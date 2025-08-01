/*
 * Copyright (c) 1997, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "compiler/compileLog.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/c2/barrierSetC2.hpp"
#include "memory/allocation.inline.hpp"
#include "opto/addnode.hpp"
#include "opto/callnode.hpp"
#include "opto/cfgnode.hpp"
#include "opto/loopnode.hpp"
#include "opto/matcher.hpp"
#include "opto/movenode.hpp"
#include "opto/mulnode.hpp"
#include "opto/opaquenode.hpp"
#include "opto/opcodes.hpp"
#include "opto/phaseX.hpp"
#include "opto/subnode.hpp"
#include "runtime/sharedRuntime.hpp"
#include "utilities/reverse_bits.hpp"

// Portions of code courtesy of Clifford Click

// Optimization - Graph Style

#include "math.h"

//=============================================================================
//------------------------------Identity---------------------------------------
// If right input is a constant 0, return the left input.
Node* SubNode::Identity(PhaseGVN* phase) {
  assert(in(1) != this, "Must already have called Value");
  assert(in(2) != this, "Must already have called Value");

  const Type* zero = add_id();

  // Remove double negation if it is not a floating point number since negation
  // is not the same as subtraction for floating point numbers
  // (cf. JLS § 15.15.4). `0-(0-(-0.0))` must be equal to positive 0.0 according to
  // JLS § 15.8.2, but would result in -0.0 if this folding would be applied.
  if (phase->type(in(1))->higher_equal(zero) &&
      in(2)->Opcode() == Opcode() &&
      phase->type(in(2)->in(1))->higher_equal(zero) &&
      !phase->type(in(2)->in(2))->is_floatingpoint()) {
    return in(2)->in(2);
  }

  // Convert "(X+Y) - Y" into X and "(X+Y) - X" into Y
  if (in(1)->Opcode() == Op_AddI || in(1)->Opcode() == Op_AddL) {
    if (in(1)->in(2) == in(2)) {
      return in(1)->in(1);
    }
    if (in(1)->in(1) == in(2)) {
      return in(1)->in(2);
    }
  }

  return ( phase->type( in(2) )->higher_equal( zero ) ) ? in(1) : this;
}

//------------------------------Value------------------------------------------
// A subtract node differences it's two inputs.
const Type* SubNode::Value_common(PhaseValues* phase) const {
  const Node* in1 = in(1);
  const Node* in2 = in(2);
  // Either input is TOP ==> the result is TOP
  const Type* t1 = (in1 == this) ? Type::TOP : phase->type(in1);
  if( t1 == Type::TOP ) return Type::TOP;
  const Type* t2 = (in2 == this) ? Type::TOP : phase->type(in2);
  if( t2 == Type::TOP ) return Type::TOP;

  // Not correct for SubFnode and AddFNode (must check for infinity)
  // Equal?  Subtract is zero
  if (in1->eqv_uncast(in2))  return add_id();

  // Either input is BOTTOM ==> the result is the local BOTTOM
  if( t1 == Type::BOTTOM || t2 == Type::BOTTOM )
    return bottom_type();

  return nullptr;
}

const Type* SubNode::Value(PhaseGVN* phase) const {
  const Type* t = Value_common(phase);
  if (t != nullptr) {
    return t;
  }
  const Type* t1 = phase->type(in(1));
  const Type* t2 = phase->type(in(2));
  return sub(t1,t2);            // Local flavor of type subtraction

}

SubNode* SubNode::make(Node* in1, Node* in2, BasicType bt) {
  switch (bt) {
    case T_INT:
      return new SubINode(in1, in2);
    case T_LONG:
      return new SubLNode(in1, in2);
    default:
      fatal("Not implemented for %s", type2name(bt));
  }
  return nullptr;
}

//=============================================================================
//------------------------------Helper function--------------------------------

static bool is_cloop_increment(Node* inc) {
  precond(inc->Opcode() == Op_AddI || inc->Opcode() == Op_AddL);

  if (!inc->in(1)->is_Phi()) {
    return false;
  }
  const PhiNode* phi = inc->in(1)->as_Phi();

  if (!phi->region()->is_CountedLoop()) {
    return false;
  }

  return inc == phi->region()->as_CountedLoop()->incr();
}

// Given the expression '(x + C) - v', or
//                      'v - (x + C)', we examine nodes '+' and 'v':
//
//  1. Do not convert if '+' is a counted-loop increment, because the '-' is
//     loop invariant and converting extends the live-range of 'x' to overlap
//     with the '+', forcing another register to be used in the loop.
//
//  2. Do not convert if 'v' is a counted-loop induction variable, because
//     'x' might be invariant.
//
static bool ok_to_convert(Node* inc, Node* var) {
  return !(is_cloop_increment(inc) || var->is_cloop_ind_var());
}

static bool is_cloop_condition(BoolNode* bol) {
  for (DUIterator_Fast imax, i = bol->fast_outs(imax); i < imax; i++) {
    Node* out = bol->fast_out(i);
    if (out->is_BaseCountedLoopEnd()) {
      return true;
    }
  }
  return false;
}

//------------------------------Ideal------------------------------------------
Node *SubINode::Ideal(PhaseGVN *phase, bool can_reshape){
  Node *in1 = in(1);
  Node *in2 = in(2);
  uint op1 = in1->Opcode();
  uint op2 = in2->Opcode();

#ifdef ASSERT
  // Check for dead loop
  if ((in1 == this) || (in2 == this) ||
      ((op1 == Op_AddI || op1 == Op_SubI) &&
       ((in1->in(1) == this) || (in1->in(2) == this) ||
        (in1->in(1) == in1)  || (in1->in(2) == in1)))) {
    assert(false, "dead loop in SubINode::Ideal");
  }
#endif

  const Type *t2 = phase->type( in2 );
  if( t2 == Type::TOP ) return nullptr;
  // Convert "x-c0" into "x+ -c0".
  if( t2->base() == Type::Int ){        // Might be bottom or top...
    const TypeInt *i = t2->is_int();
    if( i->is_con() )
      return new AddINode(in1, phase->intcon(java_negate(i->get_con())));
  }

  // Convert "(x+c0) - y" into (x-y) + c0"
  // Do not collapse (x+c0)-y if "+" is a loop increment or
  // if "y" is a loop induction variable.
  if( op1 == Op_AddI && ok_to_convert(in1, in2) ) {
    const Type *tadd = phase->type( in1->in(2) );
    if( tadd->singleton() && tadd != Type::TOP ) {
      Node *sub2 = phase->transform( new SubINode( in1->in(1), in2 ));
      return new AddINode( sub2, in1->in(2) );
    }
  }

  // Convert "x - (y+c0)" into "(x-y) - c0" AND
  // Convert "c1 - (y+c0)" into "(c1-c0) - y"
  // Need the same check as in above optimization but reversed.
  if (op2 == Op_AddI
      && ok_to_convert(in2, in1)
      && in2->in(2)->Opcode() == Op_ConI) {
    jint c0 = phase->type(in2->in(2))->isa_int()->get_con();
    Node* in21 = in2->in(1);
    if (in1->Opcode() == Op_ConI) {
      // Match c1
      jint c1 = phase->type(in1)->isa_int()->get_con();
      Node* sub2 = phase->intcon(java_subtract(c1, c0));
      return new SubINode(sub2, in21);
    } else {
      // Match x
      Node* sub2 = phase->transform(new SubINode(in1, in21));
      Node* neg_c0 = phase->intcon(java_negate(c0));
      return new AddINode(sub2, neg_c0);
    }
  }

  const Type *t1 = phase->type( in1 );
  if( t1 == Type::TOP ) return nullptr;

#ifdef ASSERT
  // Check for dead loop
  if ((op2 == Op_AddI || op2 == Op_SubI) &&
      ((in2->in(1) == this) || (in2->in(2) == this) ||
       (in2->in(1) == in2)  || (in2->in(2) == in2))) {
    assert(false, "dead loop in SubINode::Ideal");
  }
#endif

  // Convert "x - (x+y)" into "-y"
  if (op2 == Op_AddI && in1 == in2->in(1)) {
    return new SubINode(phase->intcon(0), in2->in(2));
  }
  // Convert "(x-y) - x" into "-y"
  if (op1 == Op_SubI && in1->in(1) == in2) {
    return new SubINode(phase->intcon(0), in1->in(2));
  }
  // Convert "x - (y+x)" into "-y"
  if (op2 == Op_AddI && in1 == in2->in(2)) {
    return new SubINode(phase->intcon(0), in2->in(1));
  }

  // Convert "0 - (x-y)" into "y-x", leave the double negation "-(-y)" to SubNode::Identity().
  if (t1 == TypeInt::ZERO && op2 == Op_SubI && phase->type(in2->in(1)) != TypeInt::ZERO) {
    return new SubINode(in2->in(2), in2->in(1));
  }

  // Convert "0 - (x+con)" into "-con-x"
  jint con;
  if( t1 == TypeInt::ZERO && op2 == Op_AddI &&
      (con = in2->in(2)->find_int_con(0)) != 0 )
    return new SubINode( phase->intcon(-con), in2->in(1) );

  // Convert "(X+A) - (X+B)" into "A - B"
  if( op1 == Op_AddI && op2 == Op_AddI && in1->in(1) == in2->in(1) )
    return new SubINode( in1->in(2), in2->in(2) );

  // Convert "(A+X) - (B+X)" into "A - B"
  if( op1 == Op_AddI && op2 == Op_AddI && in1->in(2) == in2->in(2) )
    return new SubINode( in1->in(1), in2->in(1) );

  // Convert "(A+X) - (X+B)" into "A - B"
  if( op1 == Op_AddI && op2 == Op_AddI && in1->in(2) == in2->in(1) )
    return new SubINode( in1->in(1), in2->in(2) );

  // Convert "(X+A) - (B+X)" into "A - B"
  if( op1 == Op_AddI && op2 == Op_AddI && in1->in(1) == in2->in(2) )
    return new SubINode( in1->in(2), in2->in(1) );

  // Convert "A-(B-C)" into (A+C)-B", since add is commutative and generally
  // nicer to optimize than subtract.
  if( op2 == Op_SubI && in2->outcnt() == 1) {
    Node *add1 = phase->transform( new AddINode( in1, in2->in(2) ) );
    return new SubINode( add1, in2->in(1) );
  }

  // Associative
  if (op1 == Op_MulI && op2 == Op_MulI) {
    Node* sub_in1 = nullptr;
    Node* sub_in2 = nullptr;
    Node* mul_in = nullptr;

    if (in1->in(1) == in2->in(1)) {
      // Convert "a*b-a*c into a*(b-c)
      sub_in1 = in1->in(2);
      sub_in2 = in2->in(2);
      mul_in = in1->in(1);
    } else if (in1->in(2) == in2->in(1)) {
      // Convert a*b-b*c into b*(a-c)
      sub_in1 = in1->in(1);
      sub_in2 = in2->in(2);
      mul_in = in1->in(2);
    } else if (in1->in(2) == in2->in(2)) {
      // Convert a*c-b*c into (a-b)*c
      sub_in1 = in1->in(1);
      sub_in2 = in2->in(1);
      mul_in = in1->in(2);
    } else if (in1->in(1) == in2->in(2)) {
      // Convert a*b-c*a into a*(b-c)
      sub_in1 = in1->in(2);
      sub_in2 = in2->in(1);
      mul_in = in1->in(1);
    }

    if (mul_in != nullptr) {
      Node* sub = phase->transform(new SubINode(sub_in1, sub_in2));
      return new MulINode(mul_in, sub);
    }
  }

  // Convert "0-(A>>31)" into "(A>>>31)"
  if ( op2 == Op_RShiftI ) {
    Node *in21 = in2->in(1);
    Node *in22 = in2->in(2);
    const TypeInt *zero = phase->type(in1)->isa_int();
    const TypeInt *t21 = phase->type(in21)->isa_int();
    const TypeInt *t22 = phase->type(in22)->isa_int();
    if ( t21 && t22 && zero == TypeInt::ZERO && t22->is_con(31) ) {
      return new URShiftINode(in21, in22);
    }
  }

  return nullptr;
}

//------------------------------sub--------------------------------------------
// A subtract node differences it's two inputs.
const Type *SubINode::sub( const Type *t1, const Type *t2 ) const {
  const TypeInt *r0 = t1->is_int(); // Handy access
  const TypeInt *r1 = t2->is_int();
  int32_t lo = java_subtract(r0->_lo, r1->_hi);
  int32_t hi = java_subtract(r0->_hi, r1->_lo);

  // We next check for 32-bit overflow.
  // If that happens, we just assume all integers are possible.
  if( (((r0->_lo ^ r1->_hi) >= 0) ||    // lo ends have same signs OR
       ((r0->_lo ^      lo) >= 0)) &&   // lo results have same signs AND
      (((r0->_hi ^ r1->_lo) >= 0) ||    // hi ends have same signs OR
       ((r0->_hi ^      hi) >= 0)) )    // hi results have same signs
    return TypeInt::make(lo,hi,MAX2(r0->_widen,r1->_widen));
  else                          // Overflow; assume all integers
    return TypeInt::INT;
}

//=============================================================================
//------------------------------Ideal------------------------------------------
Node *SubLNode::Ideal(PhaseGVN *phase, bool can_reshape) {
  Node *in1 = in(1);
  Node *in2 = in(2);
  uint op1 = in1->Opcode();
  uint op2 = in2->Opcode();

#ifdef ASSERT
  // Check for dead loop
  if ((in1 == this) || (in2 == this) ||
      ((op1 == Op_AddL || op1 == Op_SubL) &&
       ((in1->in(1) == this) || (in1->in(2) == this) ||
        (in1->in(1) == in1)  || (in1->in(2) == in1)))) {
    assert(false, "dead loop in SubLNode::Ideal");
  }
#endif

  if( phase->type( in2 ) == Type::TOP ) return nullptr;
  const TypeLong *i = phase->type( in2 )->isa_long();
  // Convert "x-c0" into "x+ -c0".
  if( i &&                      // Might be bottom or top...
      i->is_con() )
    return new AddLNode(in1, phase->longcon(java_negate(i->get_con())));

  // Convert "(x+c0) - y" into (x-y) + c0"
  // Do not collapse (x+c0)-y if "+" is a loop increment or
  // if "y" is a loop induction variable.
  if( op1 == Op_AddL && ok_to_convert(in1, in2) ) {
    Node *in11 = in1->in(1);
    const Type *tadd = phase->type( in1->in(2) );
    if( tadd->singleton() && tadd != Type::TOP ) {
      Node *sub2 = phase->transform( new SubLNode( in11, in2 ));
      return new AddLNode( sub2, in1->in(2) );
    }
  }

  // Convert "x - (y+c0)" into "(x-y) - c0" AND
  // Convert "c1 - (y+c0)" into "(c1-c0) - y"
  // Need the same check as in above optimization but reversed.
  if (op2 == Op_AddL
      && ok_to_convert(in2, in1)
      && in2->in(2)->Opcode() == Op_ConL) {
    jlong c0 = phase->type(in2->in(2))->isa_long()->get_con();
    Node* in21 = in2->in(1);
    if (in1->Opcode() == Op_ConL) {
      // Match c1
      jlong c1 = phase->type(in1)->isa_long()->get_con();
      Node* sub2 = phase->longcon(java_subtract(c1, c0));
      return new SubLNode(sub2, in21);
    } else {
      Node* sub2 = phase->transform(new SubLNode(in1, in21));
      Node* neg_c0 = phase->longcon(java_negate(c0));
      return new AddLNode(sub2, neg_c0);
    }
  }

  const Type *t1 = phase->type( in1 );
  if( t1 == Type::TOP ) return nullptr;

#ifdef ASSERT
  // Check for dead loop
  if ((op2 == Op_AddL || op2 == Op_SubL) &&
      ((in2->in(1) == this) || (in2->in(2) == this) ||
       (in2->in(1) == in2)  || (in2->in(2) == in2))) {
    assert(false, "dead loop in SubLNode::Ideal");
  }
#endif

  // Convert "x - (x+y)" into "-y"
  if (op2 == Op_AddL && in1 == in2->in(1)) {
    return new SubLNode(phase->makecon(TypeLong::ZERO), in2->in(2));
  }
  // Convert "(x-y) - x" into "-y"
  if (op1 == Op_SubL && in1->in(1) == in2) {
    return new SubLNode(phase->makecon(TypeLong::ZERO), in1->in(2));
  }
  // Convert "x - (y+x)" into "-y"
  if (op2 == Op_AddL && in1 == in2->in(2)) {
    return new SubLNode(phase->makecon(TypeLong::ZERO), in2->in(1));
  }

  // Convert "0 - (x-y)" into "y-x", leave the double negation "-(-y)" to SubNode::Identity.
  if (t1 == TypeLong::ZERO && op2 == Op_SubL && phase->type(in2->in(1)) != TypeLong::ZERO) {
    return new SubLNode(in2->in(2), in2->in(1));
  }

  // Convert "(X+A) - (X+B)" into "A - B"
  if( op1 == Op_AddL && op2 == Op_AddL && in1->in(1) == in2->in(1) )
    return new SubLNode( in1->in(2), in2->in(2) );

  // Convert "(A+X) - (B+X)" into "A - B"
  if( op1 == Op_AddL && op2 == Op_AddL && in1->in(2) == in2->in(2) )
    return new SubLNode( in1->in(1), in2->in(1) );

  // Convert "(A+X) - (X+B)" into "A - B"
  if( op1 == Op_AddL && op2 == Op_AddL && in1->in(2) == in2->in(1) )
    return new SubLNode( in1->in(1), in2->in(2) );

  // Convert "(X+A) - (B+X)" into "A - B"
  if( op1 == Op_AddL && op2 == Op_AddL && in1->in(1) == in2->in(2) )
    return new SubLNode( in1->in(2), in2->in(1) );

  // Convert "A-(B-C)" into (A+C)-B"
  if( op2 == Op_SubL && in2->outcnt() == 1) {
    Node *add1 = phase->transform( new AddLNode( in1, in2->in(2) ) );
    return new SubLNode( add1, in2->in(1) );
  }

  // Associative
  if (op1 == Op_MulL && op2 == Op_MulL) {
    Node* sub_in1 = nullptr;
    Node* sub_in2 = nullptr;
    Node* mul_in = nullptr;

    if (in1->in(1) == in2->in(1)) {
      // Convert "a*b-a*c into a*(b+c)
      sub_in1 = in1->in(2);
      sub_in2 = in2->in(2);
      mul_in = in1->in(1);
    } else if (in1->in(2) == in2->in(1)) {
      // Convert a*b-b*c into b*(a-c)
      sub_in1 = in1->in(1);
      sub_in2 = in2->in(2);
      mul_in = in1->in(2);
    } else if (in1->in(2) == in2->in(2)) {
      // Convert a*c-b*c into (a-b)*c
      sub_in1 = in1->in(1);
      sub_in2 = in2->in(1);
      mul_in = in1->in(2);
    } else if (in1->in(1) == in2->in(2)) {
      // Convert a*b-c*a into a*(b-c)
      sub_in1 = in1->in(2);
      sub_in2 = in2->in(1);
      mul_in = in1->in(1);
    }

    if (mul_in != nullptr) {
      Node* sub = phase->transform(new SubLNode(sub_in1, sub_in2));
      return new MulLNode(mul_in, sub);
    }
  }

  // Convert "0L-(A>>63)" into "(A>>>63)"
  if ( op2 == Op_RShiftL ) {
    Node *in21 = in2->in(1);
    Node *in22 = in2->in(2);
    const TypeLong *zero = phase->type(in1)->isa_long();
    const TypeLong *t21 = phase->type(in21)->isa_long();
    const TypeInt *t22 = phase->type(in22)->isa_int();
    if ( t21 && t22 && zero == TypeLong::ZERO && t22->is_con(63) ) {
      return new URShiftLNode(in21, in22);
    }
  }

  return nullptr;
}

//------------------------------sub--------------------------------------------
// A subtract node differences it's two inputs.
const Type *SubLNode::sub( const Type *t1, const Type *t2 ) const {
  const TypeLong *r0 = t1->is_long(); // Handy access
  const TypeLong *r1 = t2->is_long();
  jlong lo = java_subtract(r0->_lo, r1->_hi);
  jlong hi = java_subtract(r0->_hi, r1->_lo);

  // We next check for 32-bit overflow.
  // If that happens, we just assume all integers are possible.
  if( (((r0->_lo ^ r1->_hi) >= 0) ||    // lo ends have same signs OR
       ((r0->_lo ^      lo) >= 0)) &&   // lo results have same signs AND
      (((r0->_hi ^ r1->_lo) >= 0) ||    // hi ends have same signs OR
       ((r0->_hi ^      hi) >= 0)) )    // hi results have same signs
    return TypeLong::make(lo,hi,MAX2(r0->_widen,r1->_widen));
  else                          // Overflow; assume all integers
    return TypeLong::LONG;
}

//=============================================================================
//------------------------------Value------------------------------------------
// A subtract node differences its two inputs.
const Type* SubFPNode::Value(PhaseGVN* phase) const {
  const Node* in1 = in(1);
  const Node* in2 = in(2);
  // Either input is TOP ==> the result is TOP
  const Type* t1 = (in1 == this) ? Type::TOP : phase->type(in1);
  if( t1 == Type::TOP ) return Type::TOP;
  const Type* t2 = (in2 == this) ? Type::TOP : phase->type(in2);
  if( t2 == Type::TOP ) return Type::TOP;

  // if both operands are infinity of same sign, the result is NaN; do
  // not replace with zero
  if (t1->is_finite() && t2->is_finite() && in1 == in2) {
    return add_id();
  }

  // Either input is BOTTOM ==> the result is the local BOTTOM
  const Type *bot = bottom_type();
  if( (t1 == bot) || (t2 == bot) ||
      (t1 == Type::BOTTOM) || (t2 == Type::BOTTOM) )
    return bot;

  return sub(t1,t2);            // Local flavor of type subtraction
}


//=============================================================================
//------------------------------sub--------------------------------------------
// A subtract node differences its two inputs.
const Type* SubHFNode::sub(const Type* t1, const Type* t2) const {
  // no folding if one of operands is infinity or NaN, do not do constant folding
  if(g_isfinite(t1->getf()) && g_isfinite(t2->getf())) {
    return TypeH::make(t1->getf() - t2->getf());
  }
  else if(g_isnan(t1->getf())) {
    return t1;
  }
  else if(g_isnan(t2->getf())) {
    return t2;
  }
  else {
    return Type::HALF_FLOAT;
  }
}

//------------------------------Ideal------------------------------------------
Node *SubFNode::Ideal(PhaseGVN *phase, bool can_reshape) {
  const Type *t2 = phase->type( in(2) );
  // Convert "x-c0" into "x+ -c0".
  if( t2->base() == Type::FloatCon ) {  // Might be bottom or top...
    // return new (phase->C, 3) AddFNode(in(1), phase->makecon( TypeF::make(-t2->getf()) ) );
  }

  // Cannot replace 0.0-X with -X because a 'fsub' bytecode computes
  // 0.0-0.0 as +0.0, while a 'fneg' bytecode computes -0.0.
  //if( phase->type(in(1)) == TypeF::ZERO )
  //return new (phase->C, 2) NegFNode(in(2));

  return nullptr;
}

//------------------------------sub--------------------------------------------
// A subtract node differences its two inputs.
const Type *SubFNode::sub( const Type *t1, const Type *t2 ) const {
  // no folding if one of operands is infinity or NaN, do not do constant folding
  if( g_isfinite(t1->getf()) && g_isfinite(t2->getf()) ) {
    return TypeF::make( t1->getf() - t2->getf() );
  }
  else if( g_isnan(t1->getf()) ) {
    return t1;
  }
  else if( g_isnan(t2->getf()) ) {
    return t2;
  }
  else {
    return Type::FLOAT;
  }
}

//=============================================================================
//------------------------------Ideal------------------------------------------
Node *SubDNode::Ideal(PhaseGVN *phase, bool can_reshape){
  const Type *t2 = phase->type( in(2) );
  // Convert "x-c0" into "x+ -c0".
  if( t2->base() == Type::DoubleCon ) { // Might be bottom or top...
    // return new (phase->C, 3) AddDNode(in(1), phase->makecon( TypeD::make(-t2->getd()) ) );
  }

  // Cannot replace 0.0-X with -X because a 'dsub' bytecode computes
  // 0.0-0.0 as +0.0, while a 'dneg' bytecode computes -0.0.
  //if( phase->type(in(1)) == TypeD::ZERO )
  //return new (phase->C, 2) NegDNode(in(2));

  return nullptr;
}

//------------------------------sub--------------------------------------------
// A subtract node differences its two inputs.
const Type *SubDNode::sub( const Type *t1, const Type *t2 ) const {
  // no folding if one of operands is infinity or NaN, do not do constant folding
  if( g_isfinite(t1->getd()) && g_isfinite(t2->getd()) ) {
    return TypeD::make( t1->getd() - t2->getd() );
  }
  else if( g_isnan(t1->getd()) ) {
    return t1;
  }
  else if( g_isnan(t2->getd()) ) {
    return t2;
  }
  else {
    return Type::DOUBLE;
  }
}

//=============================================================================
//------------------------------Idealize---------------------------------------
// Unlike SubNodes, compare must still flatten return value to the
// range -1, 0, 1.
// And optimizations like those for (X + Y) - X fail if overflow happens.
Node* CmpNode::Identity(PhaseGVN* phase) {
  return this;
}

CmpNode *CmpNode::make(Node *in1, Node *in2, BasicType bt, bool unsigned_comp) {
  switch (bt) {
    case T_INT:
      if (unsigned_comp) {
        return new CmpUNode(in1, in2);
      }
      return new CmpINode(in1, in2);
    case T_LONG:
      if (unsigned_comp) {
        return new CmpULNode(in1, in2);
      }
      return new CmpLNode(in1, in2);
    case T_OBJECT:
    case T_ARRAY:
    case T_ADDRESS:
    case T_METADATA:
      return new CmpPNode(in1, in2);
    case T_NARROWOOP:
    case T_NARROWKLASS:
      return new CmpNNode(in1, in2);
    default:
      fatal("Not implemented for %s", type2name(bt));
  }
  return nullptr;
}

//=============================================================================
//------------------------------cmp--------------------------------------------
// Simplify a CmpI (compare 2 integers) node, based on local information.
// If both inputs are constants, compare them.
const Type *CmpINode::sub( const Type *t1, const Type *t2 ) const {
  const TypeInt *r0 = t1->is_int(); // Handy access
  const TypeInt *r1 = t2->is_int();

  if( r0->_hi < r1->_lo )       // Range is always low?
    return TypeInt::CC_LT;
  else if( r0->_lo > r1->_hi )  // Range is always high?
    return TypeInt::CC_GT;

  else if( r0->is_con() && r1->is_con() ) { // comparing constants?
    assert(r0->get_con() == r1->get_con(), "must be equal");
    return TypeInt::CC_EQ;      // Equal results.
  } else if( r0->_hi == r1->_lo ) // Range is never high?
    return TypeInt::CC_LE;
  else if( r0->_lo == r1->_hi ) // Range is never low?
    return TypeInt::CC_GE;
  return TypeInt::CC;           // else use worst case results
}

const Type* CmpINode::Value(PhaseGVN* phase) const {
  Node* in1 = in(1);
  Node* in2 = in(2);
  // If this test is the zero trip guard for a main or post loop, check whether, with the opaque node removed, the test
  // would constant fold so the loop is never entered. If so return the type of the test without the opaque node removed:
  // make the loop unreachable.
  // The reason for this is that the iv phi captures the bounds of the loop and if the loop becomes unreachable, it can
  // become top. In that case, the loop must be removed.
  // This is safe because:
  // - as optimizations proceed, the range of iterations executed by the main loop narrows. If no iterations remain, then
  // we're done with optimizations for that loop.
  // - the post loop is initially not reachable but as long as there's a main loop, the zero trip guard for the post
  // loop takes a phi that merges the pre and main loop's iv and can't constant fold the zero trip guard. Once, the main
  // loop is removed, there's no need to preserve the zero trip guard for the post loop anymore.
  if (in1 != nullptr && in2 != nullptr) {
    uint input = 0;
    Node* cmp = nullptr;
    BoolTest::mask test;
    if (in1->Opcode() == Op_OpaqueZeroTripGuard && phase->type(in1) != Type::TOP) {
      cmp = new CmpINode(in1->in(1), in2);
      test = ((OpaqueZeroTripGuardNode*)in1)->_loop_entered_mask;
    }
    if (in2->Opcode() == Op_OpaqueZeroTripGuard && phase->type(in2) != Type::TOP) {
      assert(cmp == nullptr, "A cmp with 2 OpaqueZeroTripGuard inputs");
      cmp = new CmpINode(in1, in2->in(1));
      test = ((OpaqueZeroTripGuardNode*)in2)->_loop_entered_mask;
    }
    if (cmp != nullptr) {
      const Type* cmp_t = cmp->Value(phase);
      const Type* t = BoolTest(test).cc2logical(cmp_t);
      cmp->destruct(phase);
      if (t == TypeInt::ZERO) {
        return cmp_t;
      }
    }
  }

  return SubNode::Value(phase);
}


// Simplify a CmpU (compare 2 integers) node, based on local information.
// If both inputs are constants, compare them.
const Type *CmpUNode::sub( const Type *t1, const Type *t2 ) const {
  assert(!t1->isa_ptr(), "obsolete usage of CmpU");

  // comparing two unsigned ints
  const TypeInt *r0 = t1->is_int();   // Handy access
  const TypeInt *r1 = t2->is_int();

  // Current installed version
  // Compare ranges for non-overlap
  juint lo0 = r0->_lo;
  juint hi0 = r0->_hi;
  juint lo1 = r1->_lo;
  juint hi1 = r1->_hi;

  // If either one has both negative and positive values,
  // it therefore contains both 0 and -1, and since [0..-1] is the
  // full unsigned range, the type must act as an unsigned bottom.
  bool bot0 = ((jint)(lo0 ^ hi0) < 0);
  bool bot1 = ((jint)(lo1 ^ hi1) < 0);

  if (bot0 || bot1) {
    // All unsigned values are LE -1 and GE 0.
    if (lo0 == 0 && hi0 == 0) {
      return TypeInt::CC_LE;            //   0 <= bot
    } else if ((jint)lo0 == -1 && (jint)hi0 == -1) {
      return TypeInt::CC_GE;            // -1 >= bot
    } else if (lo1 == 0 && hi1 == 0) {
      return TypeInt::CC_GE;            // bot >= 0
    } else if ((jint)lo1 == -1 && (jint)hi1 == -1) {
      return TypeInt::CC_LE;            // bot <= -1
    }
  } else {
    // We can use ranges of the form [lo..hi] if signs are the same.
    assert(lo0 <= hi0 && lo1 <= hi1, "unsigned ranges are valid");
    // results are reversed, '-' > '+' for unsigned compare
    if (hi0 < lo1) {
      return TypeInt::CC_LT;            // smaller
    } else if (lo0 > hi1) {
      return TypeInt::CC_GT;            // greater
    } else if (hi0 == lo1 && lo0 == hi1) {
      return TypeInt::CC_EQ;            // Equal results
    } else if (lo0 >= hi1) {
      return TypeInt::CC_GE;
    } else if (hi0 <= lo1) {
      // Check for special case in Hashtable::get.  (See below.)
      if ((jint)lo0 >= 0 && (jint)lo1 >= 0 && is_index_range_check())
        return TypeInt::CC_LT;
      return TypeInt::CC_LE;
    }
  }
  // Check for special case in Hashtable::get - the hash index is
  // mod'ed to the table size so the following range check is useless.
  // Check for: (X Mod Y) CmpU Y, where the mod result and Y both have
  // to be positive.
  // (This is a gross hack, since the sub method never
  // looks at the structure of the node in any other case.)
  if ((jint)lo0 >= 0 && (jint)lo1 >= 0 && is_index_range_check())
    return TypeInt::CC_LT;
  return TypeInt::CC;                   // else use worst case results
}

const Type* CmpUNode::Value(PhaseGVN* phase) const {
  const Type* t = SubNode::Value_common(phase);
  if (t != nullptr) {
    return t;
  }
  const Node* in1 = in(1);
  const Node* in2 = in(2);
  const Type* t1 = phase->type(in1);
  const Type* t2 = phase->type(in2);
  assert(t1->isa_int(), "CmpU has only Int type inputs");
  if (t2 == TypeInt::INT) { // Compare to bottom?
    return bottom_type();
  }

  const Type* t_sub = sub(t1, t2); // compare based on immediate inputs

  uint in1_op = in1->Opcode();
  if (in1_op == Op_AddI || in1_op == Op_SubI) {
    // The problem rise when result of AddI(SubI) may overflow
    // signed integer value. Let say the input type is
    // [256, maxint] then +128 will create 2 ranges due to
    // overflow: [minint, minint+127] and [384, maxint].
    // But C2 type system keep only 1 type range and as result
    // it use general [minint, maxint] for this case which we
    // can't optimize.
    //
    // Make 2 separate type ranges based on types of AddI(SubI) inputs
    // and compare results of their compare. If results are the same
    // CmpU node can be optimized.
    const Node* in11 = in1->in(1);
    const Node* in12 = in1->in(2);
    const Type* t11 = (in11 == in1) ? Type::TOP : phase->type(in11);
    const Type* t12 = (in12 == in1) ? Type::TOP : phase->type(in12);
    // Skip cases when input types are top or bottom.
    if ((t11 != Type::TOP) && (t11 != TypeInt::INT) &&
        (t12 != Type::TOP) && (t12 != TypeInt::INT)) {
      const TypeInt *r0 = t11->is_int();
      const TypeInt *r1 = t12->is_int();
      jlong lo_r0 = r0->_lo;
      jlong hi_r0 = r0->_hi;
      jlong lo_r1 = r1->_lo;
      jlong hi_r1 = r1->_hi;
      if (in1_op == Op_SubI) {
        jlong tmp = hi_r1;
        hi_r1 = -lo_r1;
        lo_r1 = -tmp;
        // Note, for substructing [minint,x] type range
        // long arithmetic provides correct overflow answer.
        // The confusion come from the fact that in 32-bit
        // -minint == minint but in 64-bit -minint == maxint+1.
      }
      jlong lo_long = lo_r0 + lo_r1;
      jlong hi_long = hi_r0 + hi_r1;
      int lo_tr1 = min_jint;
      int hi_tr1 = (int)hi_long;
      int lo_tr2 = (int)lo_long;
      int hi_tr2 = max_jint;
      bool underflow = lo_long != (jlong)lo_tr2;
      bool overflow  = hi_long != (jlong)hi_tr1;
      // Use sub(t1, t2) when there is no overflow (one type range)
      // or when both overflow and underflow (too complex).
      if ((underflow != overflow) && (hi_tr1 < lo_tr2)) {
        // Overflow only on one boundary, compare 2 separate type ranges.
        int w = MAX2(r0->_widen, r1->_widen); // _widen does not matter here
        const TypeInt* tr1 = TypeInt::make(lo_tr1, hi_tr1, w);
        const TypeInt* tr2 = TypeInt::make(lo_tr2, hi_tr2, w);
        const TypeInt* cmp1 = sub(tr1, t2)->is_int();
        const TypeInt* cmp2 = sub(tr2, t2)->is_int();
        // Compute union, so that cmp handles all possible results from the two cases
        const Type* t_cmp = cmp1->meet(cmp2);
        // Pick narrowest type, based on overflow computation and on immediate inputs
        return t_sub->filter(t_cmp);
      }
    }
  }

  return t_sub;
}

bool CmpUNode::is_index_range_check() const {
  // Check for the "(X ModI Y) CmpU Y" shape
  return (in(1)->Opcode() == Op_ModI &&
          in(1)->in(2)->eqv_uncast(in(2)));
}

//------------------------------Idealize---------------------------------------
Node *CmpINode::Ideal( PhaseGVN *phase, bool can_reshape ) {
  if (phase->type(in(2))->higher_equal(TypeInt::ZERO)) {
    switch (in(1)->Opcode()) {
    case Op_CmpU3:              // Collapse a CmpU3/CmpI into a CmpU
      return new CmpUNode(in(1)->in(1),in(1)->in(2));
    case Op_CmpL3:              // Collapse a CmpL3/CmpI into a CmpL
      return new CmpLNode(in(1)->in(1),in(1)->in(2));
    case Op_CmpUL3:             // Collapse a CmpUL3/CmpI into a CmpUL
      return new CmpULNode(in(1)->in(1),in(1)->in(2));
    case Op_CmpF3:              // Collapse a CmpF3/CmpI into a CmpF
      return new CmpFNode(in(1)->in(1),in(1)->in(2));
    case Op_CmpD3:              // Collapse a CmpD3/CmpI into a CmpD
      return new CmpDNode(in(1)->in(1),in(1)->in(2));
    //case Op_SubI:
      // If (x - y) cannot overflow, then ((x - y) <?> 0)
      // can be turned into (x <?> y).
      // This is handled (with more general cases) by Ideal_sub_algebra.
    }
  }
  return nullptr;                  // No change
}

Node *CmpLNode::Ideal( PhaseGVN *phase, bool can_reshape ) {
  const TypeLong *t2 = phase->type(in(2))->isa_long();
  if (Opcode() == Op_CmpL && in(1)->Opcode() == Op_ConvI2L && t2 && t2->is_con()) {
    const jlong con = t2->get_con();
    if (con >= min_jint && con <= max_jint) {
      return new CmpINode(in(1)->in(1), phase->intcon((jint)con));
    }
  }
  return nullptr;
}

//=============================================================================
// Simplify a CmpL (compare 2 longs ) node, based on local information.
// If both inputs are constants, compare them.
const Type *CmpLNode::sub( const Type *t1, const Type *t2 ) const {
  const TypeLong *r0 = t1->is_long(); // Handy access
  const TypeLong *r1 = t2->is_long();

  if( r0->_hi < r1->_lo )       // Range is always low?
    return TypeInt::CC_LT;
  else if( r0->_lo > r1->_hi )  // Range is always high?
    return TypeInt::CC_GT;

  else if( r0->is_con() && r1->is_con() ) { // comparing constants?
    assert(r0->get_con() == r1->get_con(), "must be equal");
    return TypeInt::CC_EQ;      // Equal results.
  } else if( r0->_hi == r1->_lo ) // Range is never high?
    return TypeInt::CC_LE;
  else if( r0->_lo == r1->_hi ) // Range is never low?
    return TypeInt::CC_GE;
  return TypeInt::CC;           // else use worst case results
}


// Simplify a CmpUL (compare 2 unsigned longs) node, based on local information.
// If both inputs are constants, compare them.
const Type* CmpULNode::sub(const Type* t1, const Type* t2) const {
  assert(!t1->isa_ptr(), "obsolete usage of CmpUL");

  // comparing two unsigned longs
  const TypeLong* r0 = t1->is_long();   // Handy access
  const TypeLong* r1 = t2->is_long();

  // Current installed version
  // Compare ranges for non-overlap
  julong lo0 = r0->_lo;
  julong hi0 = r0->_hi;
  julong lo1 = r1->_lo;
  julong hi1 = r1->_hi;

  // If either one has both negative and positive values,
  // it therefore contains both 0 and -1, and since [0..-1] is the
  // full unsigned range, the type must act as an unsigned bottom.
  bool bot0 = ((jlong)(lo0 ^ hi0) < 0);
  bool bot1 = ((jlong)(lo1 ^ hi1) < 0);

  if (bot0 || bot1) {
    // All unsigned values are LE -1 and GE 0.
    if (lo0 == 0 && hi0 == 0) {
      return TypeInt::CC_LE;            //   0 <= bot
    } else if ((jlong)lo0 == -1 && (jlong)hi0 == -1) {
      return TypeInt::CC_GE;            // -1 >= bot
    } else if (lo1 == 0 && hi1 == 0) {
      return TypeInt::CC_GE;            // bot >= 0
    } else if ((jlong)lo1 == -1 && (jlong)hi1 == -1) {
      return TypeInt::CC_LE;            // bot <= -1
    }
  } else {
    // We can use ranges of the form [lo..hi] if signs are the same.
    assert(lo0 <= hi0 && lo1 <= hi1, "unsigned ranges are valid");
    // results are reversed, '-' > '+' for unsigned compare
    if (hi0 < lo1) {
      return TypeInt::CC_LT;            // smaller
    } else if (lo0 > hi1) {
      return TypeInt::CC_GT;            // greater
    } else if (hi0 == lo1 && lo0 == hi1) {
      return TypeInt::CC_EQ;            // Equal results
    } else if (lo0 >= hi1) {
      return TypeInt::CC_GE;
    } else if (hi0 <= lo1) {
      return TypeInt::CC_LE;
    }
  }

  return TypeInt::CC;                   // else use worst case results
}

//=============================================================================
//------------------------------sub--------------------------------------------
// Simplify an CmpP (compare 2 pointers) node, based on local information.
// If both inputs are constants, compare them.
const Type *CmpPNode::sub( const Type *t1, const Type *t2 ) const {
  const TypePtr *r0 = t1->is_ptr(); // Handy access
  const TypePtr *r1 = t2->is_ptr();

  // Undefined inputs makes for an undefined result
  if( TypePtr::above_centerline(r0->_ptr) ||
      TypePtr::above_centerline(r1->_ptr) )
    return Type::TOP;

  if (r0 == r1 && r0->singleton()) {
    // Equal pointer constants (klasses, nulls, etc.)
    return TypeInt::CC_EQ;
  }

  // See if it is 2 unrelated classes.
  const TypeOopPtr* p0 = r0->isa_oopptr();
  const TypeOopPtr* p1 = r1->isa_oopptr();
  const TypeKlassPtr* k0 = r0->isa_klassptr();
  const TypeKlassPtr* k1 = r1->isa_klassptr();
  if ((p0 && p1) || (k0 && k1)) {
    if (p0 && p1) {
      Node* in1 = in(1)->uncast();
      Node* in2 = in(2)->uncast();
      AllocateNode* alloc1 = AllocateNode::Ideal_allocation(in1);
      AllocateNode* alloc2 = AllocateNode::Ideal_allocation(in2);
      if (MemNode::detect_ptr_independence(in1, alloc1, in2, alloc2, nullptr)) {
        return TypeInt::CC_GT;  // different pointers
      }
    }
    bool    xklass0 = p0 ? p0->klass_is_exact() : k0->klass_is_exact();
    bool    xklass1 = p1 ? p1->klass_is_exact() : k1->klass_is_exact();
    bool unrelated_classes = false;

    if ((p0 && p0->is_same_java_type_as(p1)) ||
        (k0 && k0->is_same_java_type_as(k1))) {
    } else if ((p0 && !p1->maybe_java_subtype_of(p0) && !p0->maybe_java_subtype_of(p1)) ||
               (k0 && !k1->maybe_java_subtype_of(k0) && !k0->maybe_java_subtype_of(k1))) {
      unrelated_classes = true;
    } else if ((p0 && !p1->maybe_java_subtype_of(p0)) ||
               (k0 && !k1->maybe_java_subtype_of(k0))) {
      unrelated_classes = xklass1;
    } else if ((p0 && !p0->maybe_java_subtype_of(p1)) ||
               (k0 && !k0->maybe_java_subtype_of(k1))) {
      unrelated_classes = xklass0;
    }

    if (unrelated_classes) {
      // The oops classes are known to be unrelated. If the joined PTRs of
      // two oops is not Null and not Bottom, then we are sure that one
      // of the two oops is non-null, and the comparison will always fail.
      TypePtr::PTR jp = r0->join_ptr(r1->_ptr);
      if (jp != TypePtr::Null && jp != TypePtr::BotPTR) {
        return TypeInt::CC_GT;
      }
    }
  }

  // Known constants can be compared exactly
  // Null can be distinguished from any NotNull pointers
  // Unknown inputs makes an unknown result
  if( r0->singleton() ) {
    intptr_t bits0 = r0->get_con();
    if( r1->singleton() )
      return bits0 == r1->get_con() ? TypeInt::CC_EQ : TypeInt::CC_GT;
    return ( r1->_ptr == TypePtr::NotNull && bits0==0 ) ? TypeInt::CC_GT : TypeInt::CC;
  } else if( r1->singleton() ) {
    intptr_t bits1 = r1->get_con();
    return ( r0->_ptr == TypePtr::NotNull && bits1==0 ) ? TypeInt::CC_GT : TypeInt::CC;
  } else
    return TypeInt::CC;
}

static inline Node* isa_java_mirror_load(PhaseGVN* phase, Node* n) {
  // Return the klass node for (indirect load from OopHandle)
  //   LoadBarrier?(LoadP(LoadP(AddP(foo:Klass, #java_mirror))))
  //   or null if not matching.
  BarrierSetC2* bs = BarrierSet::barrier_set()->barrier_set_c2();
    n = bs->step_over_gc_barrier(n);

  if (n->Opcode() != Op_LoadP) return nullptr;

  const TypeInstPtr* tp = phase->type(n)->isa_instptr();
  if (!tp || tp->instance_klass() != phase->C->env()->Class_klass()) return nullptr;

  Node* adr = n->in(MemNode::Address);
  // First load from OopHandle: ((OopHandle)mirror)->resolve(); may need barrier.
  if (adr->Opcode() != Op_LoadP || !phase->type(adr)->isa_rawptr()) return nullptr;
  adr = adr->in(MemNode::Address);

  intptr_t off = 0;
  Node* k = AddPNode::Ideal_base_and_offset(adr, phase, off);
  if (k == nullptr)  return nullptr;
  const TypeKlassPtr* tkp = phase->type(k)->isa_klassptr();
  if (!tkp || off != in_bytes(Klass::java_mirror_offset())) return nullptr;

  // We've found the klass node of a Java mirror load.
  return k;
}

static inline Node* isa_const_java_mirror(PhaseGVN* phase, Node* n) {
  // for ConP(Foo.class) return ConP(Foo.klass)
  // otherwise return null
  if (!n->is_Con()) return nullptr;

  const TypeInstPtr* tp = phase->type(n)->isa_instptr();
  if (!tp) return nullptr;

  ciType* mirror_type = tp->java_mirror_type();
  // TypeInstPtr::java_mirror_type() returns non-null for compile-
  // time Class constants only.
  if (!mirror_type) return nullptr;

  // x.getClass() == int.class can never be true (for all primitive types)
  // Return a ConP(null) node for this case.
  if (mirror_type->is_classless()) {
    return phase->makecon(TypePtr::NULL_PTR);
  }

  // return the ConP(Foo.klass)
  assert(mirror_type->is_klass(), "mirror_type should represent a Klass*");
  return phase->makecon(TypeKlassPtr::make(mirror_type->as_klass(), Type::trust_interfaces));
}

//------------------------------Ideal------------------------------------------
// Normalize comparisons between Java mirror loads to compare the klass instead.
//
// Also check for the case of comparing an unknown klass loaded from the primary
// super-type array vs a known klass with no subtypes.  This amounts to
// checking to see an unknown klass subtypes a known klass with no subtypes;
// this only happens on an exact match.  We can shorten this test by 1 load.
Node *CmpPNode::Ideal( PhaseGVN *phase, bool can_reshape ) {
  // Normalize comparisons between Java mirrors into comparisons of the low-
  // level klass, where a dependent load could be shortened.
  //
  // The new pattern has a nice effect of matching the same pattern used in the
  // fast path of instanceof/checkcast/Class.isInstance(), which allows
  // redundant exact type check be optimized away by GVN.
  // For example, in
  //   if (x.getClass() == Foo.class) {
  //     Foo foo = (Foo) x;
  //     // ... use a ...
  //   }
  // a CmpPNode could be shared between if_acmpne and checkcast
  {
    Node* k1 = isa_java_mirror_load(phase, in(1));
    Node* k2 = isa_java_mirror_load(phase, in(2));
    Node* conk2 = isa_const_java_mirror(phase, in(2));

    if (k1 && (k2 || conk2)) {
      Node* lhs = k1;
      Node* rhs = (k2 != nullptr) ? k2 : conk2;
      set_req_X(1, lhs, phase);
      set_req_X(2, rhs, phase);
      return this;
    }
  }

  // Constant pointer on right?
  const TypeKlassPtr* t2 = phase->type(in(2))->isa_klassptr();
  if (t2 == nullptr || !t2->klass_is_exact())
    return nullptr;
  // Get the constant klass we are comparing to.
  ciKlass* superklass = t2->exact_klass();

  // Now check for LoadKlass on left.
  Node* ldk1 = in(1);
  if (ldk1->is_DecodeNKlass()) {
    ldk1 = ldk1->in(1);
    if (ldk1->Opcode() != Op_LoadNKlass )
      return nullptr;
  } else if (ldk1->Opcode() != Op_LoadKlass )
    return nullptr;
  // Take apart the address of the LoadKlass:
  Node* adr1 = ldk1->in(MemNode::Address);
  intptr_t con2 = 0;
  Node* ldk2 = AddPNode::Ideal_base_and_offset(adr1, phase, con2);
  if (ldk2 == nullptr)
    return nullptr;
  if (con2 == oopDesc::klass_offset_in_bytes()) {
    // We are inspecting an object's concrete class.
    // Short-circuit the check if the query is abstract.
    if (superklass->is_interface() ||
        superklass->is_abstract()) {
      // Make it come out always false:
      this->set_req(2, phase->makecon(TypePtr::NULL_PTR));
      return this;
    }
  }

  // Check for a LoadKlass from primary supertype array.
  // Any nested loadklass from loadklass+con must be from the p.s. array.
  if (ldk2->is_DecodeNKlass()) {
    // Keep ldk2 as DecodeN since it could be used in CmpP below.
    if (ldk2->in(1)->Opcode() != Op_LoadNKlass )
      return nullptr;
  } else if (ldk2->Opcode() != Op_LoadKlass)
    return nullptr;

  // Verify that we understand the situation
  if (con2 != (intptr_t) superklass->super_check_offset())
    return nullptr;                // Might be element-klass loading from array klass

  // If 'superklass' has no subklasses and is not an interface, then we are
  // assured that the only input which will pass the type check is
  // 'superklass' itself.
  //
  // We could be more liberal here, and allow the optimization on interfaces
  // which have a single implementor.  This would require us to increase the
  // expressiveness of the add_dependency() mechanism.
  // %%% Do this after we fix TypeOopPtr:  Deps are expressive enough now.

  // Object arrays must have their base element have no subtypes
  while (superklass->is_obj_array_klass()) {
    ciType* elem = superklass->as_obj_array_klass()->element_type();
    superklass = elem->as_klass();
  }
  if (superklass->is_instance_klass()) {
    ciInstanceKlass* ik = superklass->as_instance_klass();
    if (ik->has_subklass() || ik->is_interface())  return nullptr;
    // Add a dependency if there is a chance that a subclass will be added later.
    if (!ik->is_final()) {
      phase->C->dependencies()->assert_leaf_type(ik);
    }
  }

  // Bypass the dependent load, and compare directly
  this->set_req_X(1, ldk2, phase);

  return this;
}

//=============================================================================
//------------------------------sub--------------------------------------------
// Simplify an CmpN (compare 2 pointers) node, based on local information.
// If both inputs are constants, compare them.
const Type *CmpNNode::sub( const Type *t1, const Type *t2 ) const {
  ShouldNotReachHere();
  return bottom_type();
}

//------------------------------Ideal------------------------------------------
Node *CmpNNode::Ideal( PhaseGVN *phase, bool can_reshape ) {
  return nullptr;
}

//=============================================================================
//------------------------------Value------------------------------------------
// Simplify an CmpF (compare 2 floats ) node, based on local information.
// If both inputs are constants, compare them.
const Type* CmpFNode::Value(PhaseGVN* phase) const {
  const Node* in1 = in(1);
  const Node* in2 = in(2);
  // Either input is TOP ==> the result is TOP
  const Type* t1 = (in1 == this) ? Type::TOP : phase->type(in1);
  if( t1 == Type::TOP ) return Type::TOP;
  const Type* t2 = (in2 == this) ? Type::TOP : phase->type(in2);
  if( t2 == Type::TOP ) return Type::TOP;

  // Not constants?  Don't know squat - even if they are the same
  // value!  If they are NaN's they compare to LT instead of EQ.
  const TypeF *tf1 = t1->isa_float_constant();
  const TypeF *tf2 = t2->isa_float_constant();
  if( !tf1 || !tf2 ) return TypeInt::CC;

  // This implements the Java bytecode fcmpl, so unordered returns -1.
  if( tf1->is_nan() || tf2->is_nan() )
    return TypeInt::CC_LT;

  if( tf1->_f < tf2->_f ) return TypeInt::CC_LT;
  if( tf1->_f > tf2->_f ) return TypeInt::CC_GT;
  assert( tf1->_f == tf2->_f, "do not understand FP behavior" );
  return TypeInt::CC_EQ;
}


//=============================================================================
//------------------------------Value------------------------------------------
// Simplify an CmpD (compare 2 doubles ) node, based on local information.
// If both inputs are constants, compare them.
const Type* CmpDNode::Value(PhaseGVN* phase) const {
  const Node* in1 = in(1);
  const Node* in2 = in(2);
  // Either input is TOP ==> the result is TOP
  const Type* t1 = (in1 == this) ? Type::TOP : phase->type(in1);
  if( t1 == Type::TOP ) return Type::TOP;
  const Type* t2 = (in2 == this) ? Type::TOP : phase->type(in2);
  if( t2 == Type::TOP ) return Type::TOP;

  // Not constants?  Don't know squat - even if they are the same
  // value!  If they are NaN's they compare to LT instead of EQ.
  const TypeD *td1 = t1->isa_double_constant();
  const TypeD *td2 = t2->isa_double_constant();
  if( !td1 || !td2 ) return TypeInt::CC;

  // This implements the Java bytecode dcmpl, so unordered returns -1.
  if( td1->is_nan() || td2->is_nan() )
    return TypeInt::CC_LT;

  if( td1->_d < td2->_d ) return TypeInt::CC_LT;
  if( td1->_d > td2->_d ) return TypeInt::CC_GT;
  assert( td1->_d == td2->_d, "do not understand FP behavior" );
  return TypeInt::CC_EQ;
}

//------------------------------Ideal------------------------------------------
Node *CmpDNode::Ideal(PhaseGVN *phase, bool can_reshape){
  // Check if we can change this to a CmpF and remove a ConvD2F operation.
  // Change  (CMPD (F2D (float)) (ConD value))
  // To      (CMPF      (float)  (ConF value))
  // Valid when 'value' does not lose precision as a float.
  // Benefits: eliminates conversion, does not require 24-bit mode

  // NaNs prevent commuting operands.  This transform works regardless of the
  // order of ConD and ConvF2D inputs by preserving the original order.
  int idx_f2d = 1;              // ConvF2D on left side?
  if( in(idx_f2d)->Opcode() != Op_ConvF2D )
    idx_f2d = 2;                // No, swap to check for reversed args
  int idx_con = 3-idx_f2d;      // Check for the constant on other input

  if( ConvertCmpD2CmpF &&
      in(idx_f2d)->Opcode() == Op_ConvF2D &&
      in(idx_con)->Opcode() == Op_ConD ) {
    const TypeD *t2 = in(idx_con)->bottom_type()->is_double_constant();
    double t2_value_as_double = t2->_d;
    float  t2_value_as_float  = (float)t2_value_as_double;
    if( t2_value_as_double == (double)t2_value_as_float ) {
      // Test value can be represented as a float
      // Eliminate the conversion to double and create new comparison
      Node *new_in1 = in(idx_f2d)->in(1);
      Node *new_in2 = phase->makecon( TypeF::make(t2_value_as_float) );
      if( idx_f2d != 1 ) {      // Must flip args to match original order
        Node *tmp = new_in1;
        new_in1 = new_in2;
        new_in2 = tmp;
      }
      CmpFNode *new_cmp = (Opcode() == Op_CmpD3)
        ? new CmpF3Node( new_in1, new_in2 )
        : new CmpFNode ( new_in1, new_in2 ) ;
      return new_cmp;           // Changed to CmpFNode
    }
    // Testing value required the precision of a double
  }
  return nullptr;                  // No change
}


//=============================================================================
//------------------------------cc2logical-------------------------------------
// Convert a condition code type to a logical type
const Type *BoolTest::cc2logical( const Type *CC ) const {
  if( CC == Type::TOP ) return Type::TOP;
  if( CC->base() != Type::Int ) return TypeInt::BOOL; // Bottom or worse
  const TypeInt *ti = CC->is_int();
  if( ti->is_con() ) {          // Only 1 kind of condition codes set?
    // Match low order 2 bits
    int tmp = ((ti->get_con()&3) == (_test&3)) ? 1 : 0;
    if( _test & 4 ) tmp = 1-tmp;     // Optionally complement result
    return TypeInt::make(tmp);       // Boolean result
  }

  if( CC == TypeInt::CC_GE ) {
    if( _test == ge ) return TypeInt::ONE;
    if( _test == lt ) return TypeInt::ZERO;
  }
  if( CC == TypeInt::CC_LE ) {
    if( _test == le ) return TypeInt::ONE;
    if( _test == gt ) return TypeInt::ZERO;
  }

  return TypeInt::BOOL;
}

//------------------------------dump_spec-------------------------------------
// Print special per-node info
void BoolTest::dump_on(outputStream *st) const {
  const char *msg[] = {"eq","gt","of","lt","ne","le","nof","ge"};
  st->print("%s", msg[_test]);
}

// Returns the logical AND of two tests (or 'never' if both tests can never be true).
// For example, a test for 'le' followed by a test for 'lt' is equivalent with 'lt'.
BoolTest::mask BoolTest::merge(BoolTest other) const {
  const mask res[illegal+1][illegal+1] = {
    // eq,      gt,      of,      lt,      ne,      le,      nof,     ge,      never,   illegal
      {eq,      never,   illegal, never,   never,   eq,      illegal, eq,      never,   illegal},  // eq
      {never,   gt,      illegal, never,   gt,      never,   illegal, gt,      never,   illegal},  // gt
      {illegal, illegal, illegal, illegal, illegal, illegal, illegal, illegal, never,   illegal},  // of
      {never,   never,   illegal, lt,      lt,      lt,      illegal, never,   never,   illegal},  // lt
      {never,   gt,      illegal, lt,      ne,      lt,      illegal, gt,      never,   illegal},  // ne
      {eq,      never,   illegal, lt,      lt,      le,      illegal, eq,      never,   illegal},  // le
      {illegal, illegal, illegal, illegal, illegal, illegal, illegal, illegal, never,   illegal},  // nof
      {eq,      gt,      illegal, never,   gt,      eq,      illegal, ge,      never,   illegal},  // ge
      {never,   never,   never,   never,   never,   never,   never,   never,   never,   illegal},  // never
      {illegal, illegal, illegal, illegal, illegal, illegal, illegal, illegal, illegal, illegal}}; // illegal
  return res[_test][other._test];
}

//=============================================================================
uint BoolNode::hash() const { return (Node::hash() << 3)|(_test._test+1); }
uint BoolNode::size_of() const { return sizeof(BoolNode); }

//------------------------------operator==-------------------------------------
bool BoolNode::cmp( const Node &n ) const {
  const BoolNode *b = (const BoolNode *)&n; // Cast up
  return (_test._test == b->_test._test);
}

//-------------------------------make_predicate--------------------------------
Node* BoolNode::make_predicate(Node* test_value, PhaseGVN* phase) {
  if (test_value->is_Con())   return test_value;
  if (test_value->is_Bool())  return test_value;
  if (test_value->is_CMove() &&
      test_value->in(CMoveNode::Condition)->is_Bool()) {
    BoolNode*   bol   = test_value->in(CMoveNode::Condition)->as_Bool();
    const Type* ftype = phase->type(test_value->in(CMoveNode::IfFalse));
    const Type* ttype = phase->type(test_value->in(CMoveNode::IfTrue));
    if (ftype == TypeInt::ZERO && !TypeInt::ZERO->higher_equal(ttype)) {
      return bol;
    } else if (ttype == TypeInt::ZERO && !TypeInt::ZERO->higher_equal(ftype)) {
      return phase->transform( bol->negate(phase) );
    }
    // Else fall through.  The CMove gets in the way of the test.
    // It should be the case that make_predicate(bol->as_int_value()) == bol.
  }
  Node* cmp = new CmpINode(test_value, phase->intcon(0));
  cmp = phase->transform(cmp);
  Node* bol = new BoolNode(cmp, BoolTest::ne);
  return phase->transform(bol);
}

//--------------------------------as_int_value---------------------------------
Node* BoolNode::as_int_value(PhaseGVN* phase) {
  // Inverse to make_predicate.  The CMove probably boils down to a Conv2B.
  Node* cmov = CMoveNode::make(this, phase->intcon(0), phase->intcon(1), TypeInt::BOOL);
  return phase->transform(cmov);
}

//----------------------------------negate-------------------------------------
BoolNode* BoolNode::negate(PhaseGVN* phase) {
  return new BoolNode(in(1), _test.negate());
}

// Change "bool eq/ne (cmp (add/sub A B) C)" into false/true if add/sub
// overflows and we can prove that C is not in the two resulting ranges.
// This optimization is similar to the one performed by CmpUNode::Value().
Node* BoolNode::fold_cmpI(PhaseGVN* phase, SubNode* cmp, Node* cmp1, int cmp_op,
                          int cmp1_op, const TypeInt* cmp2_type) {
  // Only optimize eq/ne integer comparison of add/sub
  if((_test._test == BoolTest::eq || _test._test == BoolTest::ne) &&
     (cmp_op == Op_CmpI) && (cmp1_op == Op_AddI || cmp1_op == Op_SubI)) {
    // Skip cases were inputs of add/sub are not integers or of bottom type
    const TypeInt* r0 = phase->type(cmp1->in(1))->isa_int();
    const TypeInt* r1 = phase->type(cmp1->in(2))->isa_int();
    if ((r0 != nullptr) && (r0 != TypeInt::INT) &&
        (r1 != nullptr) && (r1 != TypeInt::INT) &&
        (cmp2_type != TypeInt::INT)) {
      // Compute exact (long) type range of add/sub result
      jlong lo_long = r0->_lo;
      jlong hi_long = r0->_hi;
      if (cmp1_op == Op_AddI) {
        lo_long += r1->_lo;
        hi_long += r1->_hi;
      } else {
        lo_long -= r1->_hi;
        hi_long -= r1->_lo;
      }
      // Check for over-/underflow by casting to integer
      int lo_int = (int)lo_long;
      int hi_int = (int)hi_long;
      bool underflow = lo_long != (jlong)lo_int;
      bool overflow  = hi_long != (jlong)hi_int;
      if ((underflow != overflow) && (hi_int < lo_int)) {
        // Overflow on one boundary, compute resulting type ranges:
        // tr1 [MIN_INT, hi_int] and tr2 [lo_int, MAX_INT]
        int w = MAX2(r0->_widen, r1->_widen); // _widen does not matter here
        const TypeInt* tr1 = TypeInt::make(min_jint, hi_int, w);
        const TypeInt* tr2 = TypeInt::make(lo_int, max_jint, w);
        // Compare second input of cmp to both type ranges
        const Type* sub_tr1 = cmp->sub(tr1, cmp2_type);
        const Type* sub_tr2 = cmp->sub(tr2, cmp2_type);
        if (sub_tr1 == TypeInt::CC_LT && sub_tr2 == TypeInt::CC_GT) {
          // The result of the add/sub will never equal cmp2. Replace BoolNode
          // by false (0) if it tests for equality and by true (1) otherwise.
          return ConINode::make((_test._test == BoolTest::eq) ? 0 : 1);
        }
      }
    }
  }
  return nullptr;
}

static bool is_counted_loop_cmp(Node *cmp) {
  Node *n = cmp->in(1)->in(1);
  return n != nullptr &&
         n->is_Phi() &&
         n->in(0) != nullptr &&
         n->in(0)->is_CountedLoop() &&
         n->in(0)->as_CountedLoop()->phi() == n;
}

//------------------------------Ideal------------------------------------------
Node *BoolNode::Ideal(PhaseGVN *phase, bool can_reshape) {
  // Change "bool tst (cmp con x)" into "bool ~tst (cmp x con)".
  // This moves the constant to the right.  Helps value-numbering.
  Node *cmp = in(1);
  if( !cmp->is_Sub() ) return nullptr;
  int cop = cmp->Opcode();
  if( cop == Op_FastLock || cop == Op_FastUnlock ||
      cmp->is_SubTypeCheck() || cop == Op_VectorTest ) {
    return nullptr;
  }
  Node *cmp1 = cmp->in(1);
  Node *cmp2 = cmp->in(2);
  if( !cmp1 ) return nullptr;

  if (_test._test == BoolTest::overflow || _test._test == BoolTest::no_overflow) {
    return nullptr;
  }

  const int cmp1_op = cmp1->Opcode();
  const int cmp2_op = cmp2->Opcode();

  // Constant on left?
  Node *con = cmp1;
  // Move constants to the right of compare's to canonicalize.
  // Do not muck with Opaque1 nodes, as this indicates a loop
  // guard that cannot change shape.
  if (con->is_Con() && !cmp2->is_Con() && cmp2_op != Op_OpaqueZeroTripGuard &&
      // Because of NaN's, CmpD and CmpF are not commutative
      cop != Op_CmpD && cop != Op_CmpF &&
      // Protect against swapping inputs to a compare when it is used by a
      // counted loop exit, which requires maintaining the loop-limit as in(2)
      !is_counted_loop_exit_test() ) {
    // Ok, commute the constant to the right of the cmp node.
    // Clone the Node, getting a new Node of the same class
    cmp = cmp->clone();
    // Swap inputs to the clone
    cmp->swap_edges(1, 2);
    cmp = phase->transform( cmp );
    return new BoolNode( cmp, _test.commute() );
  }

  // Change "bool eq/ne (cmp (cmove (bool tst (cmp2)) 1 0) 0)" into "bool tst/~tst (cmp2)"
  if (cop == Op_CmpI &&
      (_test._test == BoolTest::eq || _test._test == BoolTest::ne) &&
      cmp1_op == Op_CMoveI && cmp2->find_int_con(1) == 0) {
    // 0 should be on the true branch
    if (cmp1->in(CMoveNode::Condition)->is_Bool() &&
        cmp1->in(CMoveNode::IfTrue)->find_int_con(1) == 0 &&
        cmp1->in(CMoveNode::IfFalse)->find_int_con(0) != 0) {
      BoolNode* target = cmp1->in(CMoveNode::Condition)->as_Bool();
      return new BoolNode(target->in(1),
                          (_test._test == BoolTest::eq) ? target->_test._test :
                                                          target->_test.negate());
    }
  }

  // Change "bool eq/ne (cmp (and X 16) 16)" into "bool ne/eq (cmp (and X 16) 0)".
  if (cop == Op_CmpI &&
      (_test._test == BoolTest::eq || _test._test == BoolTest::ne) &&
      cmp1_op == Op_AndI && cmp2_op == Op_ConI &&
      cmp1->in(2)->Opcode() == Op_ConI) {
    const TypeInt *t12 = phase->type(cmp2)->isa_int();
    const TypeInt *t112 = phase->type(cmp1->in(2))->isa_int();
    if (t12 && t12->is_con() && t112 && t112->is_con() &&
        t12->get_con() == t112->get_con() && is_power_of_2(t12->get_con())) {
      Node *ncmp = phase->transform(new CmpINode(cmp1, phase->intcon(0)));
      return new BoolNode(ncmp, _test.negate());
    }
  }

  // Same for long type: change "bool eq/ne (cmp (and X 16) 16)" into "bool ne/eq (cmp (and X 16) 0)".
  if (cop == Op_CmpL &&
      (_test._test == BoolTest::eq || _test._test == BoolTest::ne) &&
      cmp1_op == Op_AndL && cmp2_op == Op_ConL &&
      cmp1->in(2)->Opcode() == Op_ConL) {
    const TypeLong *t12 = phase->type(cmp2)->isa_long();
    const TypeLong *t112 = phase->type(cmp1->in(2))->isa_long();
    if (t12 && t12->is_con() && t112 && t112->is_con() &&
        t12->get_con() == t112->get_con() && is_power_of_2(t12->get_con())) {
      Node *ncmp = phase->transform(new CmpLNode(cmp1, phase->longcon(0)));
      return new BoolNode(ncmp, _test.negate());
    }
  }

  // Change "cmp (add X min_jint) (add Y min_jint)" into "cmpu X Y"
  // and    "cmp (add X min_jint) c" into "cmpu X (c + min_jint)"
  if (cop == Op_CmpI &&
      cmp1_op == Op_AddI &&
      phase->type(cmp1->in(2)) == TypeInt::MIN &&
      !is_cloop_condition(this)) {
    if (cmp2_op == Op_ConI) {
      Node* ncmp2 = phase->intcon(java_add(cmp2->get_int(), min_jint));
      Node* ncmp = phase->transform(new CmpUNode(cmp1->in(1), ncmp2));
      return new BoolNode(ncmp, _test._test);
    } else if (cmp2_op == Op_AddI &&
               phase->type(cmp2->in(2)) == TypeInt::MIN &&
               !is_cloop_condition(this)) {
      Node* ncmp = phase->transform(new CmpUNode(cmp1->in(1), cmp2->in(1)));
      return new BoolNode(ncmp, _test._test);
    }
  }

  // Change "cmp (add X min_jlong) (add Y min_jlong)" into "cmpu X Y"
  // and    "cmp (add X min_jlong) c" into "cmpu X (c + min_jlong)"
  if (cop == Op_CmpL &&
      cmp1_op == Op_AddL &&
      phase->type(cmp1->in(2)) == TypeLong::MIN &&
      !is_cloop_condition(this)) {
    if (cmp2_op == Op_ConL) {
      Node* ncmp2 = phase->longcon(java_add(cmp2->get_long(), min_jlong));
      Node* ncmp = phase->transform(new CmpULNode(cmp1->in(1), ncmp2));
      return new BoolNode(ncmp, _test._test);
    } else if (cmp2_op == Op_AddL &&
               phase->type(cmp2->in(2)) == TypeLong::MIN &&
               !is_cloop_condition(this)) {
      Node* ncmp = phase->transform(new CmpULNode(cmp1->in(1), cmp2->in(1)));
      return new BoolNode(ncmp, _test._test);
    }
  }

  // Change "bool eq/ne (cmp (xor X 1) 0)" into "bool ne/eq (cmp X 0)".
  // The XOR-1 is an idiom used to flip the sense of a bool.  We flip the
  // test instead.
  const TypeInt* cmp2_type = phase->type(cmp2)->isa_int();
  if (cmp2_type == nullptr)  return nullptr;
  Node* j_xor = cmp1;
  if( cmp2_type == TypeInt::ZERO &&
      cmp1_op == Op_XorI &&
      j_xor->in(1) != j_xor &&          // An xor of itself is dead
      phase->type( j_xor->in(1) ) == TypeInt::BOOL &&
      phase->type( j_xor->in(2) ) == TypeInt::ONE &&
      (_test._test == BoolTest::eq ||
       _test._test == BoolTest::ne) ) {
    Node *ncmp = phase->transform(new CmpINode(j_xor->in(1),cmp2));
    return new BoolNode( ncmp, _test.negate() );
  }

  // Transform: "((x & (m - 1)) <u m)" or "(((m - 1) & x) <u m)" into "(m >u 0)"
  // This is case [CMPU_MASK] which is further described at the method comment of BoolNode::Value_cmpu_and_mask().
  if (cop == Op_CmpU && _test._test == BoolTest::lt && cmp1_op == Op_AndI) {
    Node* m = cmp2; // RHS: m
    for (int add_idx = 1; add_idx <= 2; add_idx++) { // LHS: "(m + (-1)) & x" or "x & (m + (-1))"?
      Node* maybe_m_minus_1 = cmp1->in(add_idx);
      if (maybe_m_minus_1->Opcode() == Op_AddI &&
          maybe_m_minus_1->in(2)->find_int_con(0) == -1 &&
          maybe_m_minus_1->in(1) == m) {
        Node* m_cmpu_0 = phase->transform(new CmpUNode(m, phase->intcon(0)));
        return new BoolNode(m_cmpu_0, BoolTest::gt);
      }
    }
  }

  // Change x u< 1 or x u<= 0 to x == 0
  // and    x u> 0 or u>= 1   to x != 0
  if (cop == Op_CmpU &&
      cmp1_op != Op_LoadRange &&
      (((_test._test == BoolTest::lt || _test._test == BoolTest::ge) &&
        cmp2->find_int_con(-1) == 1) ||
       ((_test._test == BoolTest::le || _test._test == BoolTest::gt) &&
        cmp2->find_int_con(-1) == 0))) {
    Node* ncmp = phase->transform(new CmpINode(cmp1, phase->intcon(0)));
    return new BoolNode(ncmp, _test.is_less() ? BoolTest::eq : BoolTest::ne);
  }

  // Change (arraylength <= 0) or (arraylength == 0)
  //   into (arraylength u<= 0)
  // Also change (arraylength != 0) into (arraylength u> 0)
  // The latter version matches the code pattern generated for
  // array range checks, which will more likely be optimized later.
  if (cop == Op_CmpI &&
      cmp1_op == Op_LoadRange &&
      cmp2->find_int_con(-1) == 0) {
    if (_test._test == BoolTest::le || _test._test == BoolTest::eq) {
      Node* ncmp = phase->transform(new CmpUNode(cmp1, cmp2));
      return new BoolNode(ncmp, BoolTest::le);
    } else if (_test._test == BoolTest::ne) {
      Node* ncmp = phase->transform(new CmpUNode(cmp1, cmp2));
      return new BoolNode(ncmp, BoolTest::gt);
    }
  }

  // Change "bool eq/ne (cmp (Conv2B X) 0)" into "bool eq/ne (cmp X 0)".
  // This is a standard idiom for branching on a boolean value.
  Node *c2b = cmp1;
  if( cmp2_type == TypeInt::ZERO &&
      cmp1_op == Op_Conv2B &&
      (_test._test == BoolTest::eq ||
       _test._test == BoolTest::ne) ) {
    Node *ncmp = phase->transform(phase->type(c2b->in(1))->isa_int()
       ? (Node*)new CmpINode(c2b->in(1),cmp2)
       : (Node*)new CmpPNode(c2b->in(1),phase->makecon(TypePtr::NULL_PTR))
    );
    return new BoolNode( ncmp, _test._test );
  }

  // Comparing a SubI against a zero is equal to comparing the SubI
  // arguments directly.  This only works for eq and ne comparisons
  // due to possible integer overflow.
  if ((_test._test == BoolTest::eq || _test._test == BoolTest::ne) &&
        (cop == Op_CmpI) &&
        (cmp1_op == Op_SubI) &&
        ( cmp2_type == TypeInt::ZERO ) ) {
    Node *ncmp = phase->transform( new CmpINode(cmp1->in(1),cmp1->in(2)));
    return new BoolNode( ncmp, _test._test );
  }

  // Same as above but with and AddI of a constant
  if ((_test._test == BoolTest::eq || _test._test == BoolTest::ne) &&
      cop == Op_CmpI &&
      cmp1_op == Op_AddI &&
      cmp1->in(2) != nullptr &&
      phase->type(cmp1->in(2))->isa_int() &&
      phase->type(cmp1->in(2))->is_int()->is_con() &&
      cmp2_type == TypeInt::ZERO &&
      !is_counted_loop_cmp(cmp) // modifying the exit test of a counted loop messes the counted loop shape
      ) {
    const TypeInt* cmp1_in2 = phase->type(cmp1->in(2))->is_int();
    Node *ncmp = phase->transform( new CmpINode(cmp1->in(1),phase->intcon(-cmp1_in2->_hi)));
    return new BoolNode( ncmp, _test._test );
  }

  // Change "bool eq/ne (cmp (phi (X -X) 0))" into "bool eq/ne (cmp X 0)"
  // since zero check of conditional negation of an integer is equal to
  // zero check of the integer directly.
  if ((_test._test == BoolTest::eq || _test._test == BoolTest::ne) &&
      (cop == Op_CmpI) &&
      (cmp2_type == TypeInt::ZERO) &&
      (cmp1_op == Op_Phi)) {
    // There should be a diamond phi with true path at index 1 or 2
    PhiNode *phi = cmp1->as_Phi();
    int idx_true = phi->is_diamond_phi();
    if (idx_true != 0) {
      // True input is in(idx_true) while false input is in(3 - idx_true)
      Node *tin = phi->in(idx_true);
      Node *fin = phi->in(3 - idx_true);
      if ((tin->Opcode() == Op_SubI) &&
          (phase->type(tin->in(1)) == TypeInt::ZERO) &&
          (tin->in(2) == fin)) {
        // Found conditional negation at true path, create a new CmpINode without that
        Node *ncmp = phase->transform(new CmpINode(fin, cmp2));
        return new BoolNode(ncmp, _test._test);
      }
      if ((fin->Opcode() == Op_SubI) &&
          (phase->type(fin->in(1)) == TypeInt::ZERO) &&
          (fin->in(2) == tin)) {
        // Found conditional negation at false path, create a new CmpINode without that
        Node *ncmp = phase->transform(new CmpINode(tin, cmp2));
        return new BoolNode(ncmp, _test._test);
      }
    }
  }

  // Change (-A vs 0) into (A vs 0) by commuting the test.  Disallow in the
  // most general case because negating 0x80000000 does nothing.  Needed for
  // the CmpF3/SubI/CmpI idiom.
  if( cop == Op_CmpI &&
      cmp1_op == Op_SubI &&
      cmp2_type == TypeInt::ZERO &&
      phase->type( cmp1->in(1) ) == TypeInt::ZERO &&
      phase->type( cmp1->in(2) )->higher_equal(TypeInt::SYMINT) ) {
    Node *ncmp = phase->transform( new CmpINode(cmp1->in(2),cmp2));
    return new BoolNode( ncmp, _test.commute() );
  }

  // Try to optimize signed integer comparison
  return fold_cmpI(phase, cmp->as_Sub(), cmp1, cop, cmp1_op, cmp2_type);

  //  The transformation below is not valid for either signed or unsigned
  //  comparisons due to wraparound concerns at MAX_VALUE and MIN_VALUE.
  //  This transformation can be resurrected when we are able to
  //  make inferences about the range of values being subtracted from
  //  (or added to) relative to the wraparound point.
  //
  //    // Remove +/-1's if possible.
  //    // "X <= Y-1" becomes "X <  Y"
  //    // "X+1 <= Y" becomes "X <  Y"
  //    // "X <  Y+1" becomes "X <= Y"
  //    // "X-1 <  Y" becomes "X <= Y"
  //    // Do not this to compares off of the counted-loop-end.  These guys are
  //    // checking the trip counter and they want to use the post-incremented
  //    // counter.  If they use the PRE-incremented counter, then the counter has
  //    // to be incremented in a private block on a loop backedge.
  //    if( du && du->cnt(this) && du->out(this)[0]->Opcode() == Op_CountedLoopEnd )
  //      return nullptr;
  //  #ifndef PRODUCT
  //    // Do not do this in a wash GVN pass during verification.
  //    // Gets triggered by too many simple optimizations to be bothered with
  //    // re-trying it again and again.
  //    if( !phase->allow_progress() ) return nullptr;
  //  #endif
  //    // Not valid for unsigned compare because of corner cases in involving zero.
  //    // For example, replacing "X-1 <u Y" with "X <=u Y" fails to throw an
  //    // exception in case X is 0 (because 0-1 turns into 4billion unsigned but
  //    // "0 <=u Y" is always true).
  //    if( cmp->Opcode() == Op_CmpU ) return nullptr;
  //    int cmp2_op = cmp2->Opcode();
  //    if( _test._test == BoolTest::le ) {
  //      if( cmp1_op == Op_AddI &&
  //          phase->type( cmp1->in(2) ) == TypeInt::ONE )
  //        return clone_cmp( cmp, cmp1->in(1), cmp2, phase, BoolTest::lt );
  //      else if( cmp2_op == Op_AddI &&
  //         phase->type( cmp2->in(2) ) == TypeInt::MINUS_1 )
  //        return clone_cmp( cmp, cmp1, cmp2->in(1), phase, BoolTest::lt );
  //    } else if( _test._test == BoolTest::lt ) {
  //      if( cmp1_op == Op_AddI &&
  //          phase->type( cmp1->in(2) ) == TypeInt::MINUS_1 )
  //        return clone_cmp( cmp, cmp1->in(1), cmp2, phase, BoolTest::le );
  //      else if( cmp2_op == Op_AddI &&
  //         phase->type( cmp2->in(2) ) == TypeInt::ONE )
  //        return clone_cmp( cmp, cmp1, cmp2->in(1), phase, BoolTest::le );
  //    }
}

// We use the following Lemmas/insights for the following two transformations (1) and (2):
//   x & y <=u y, for any x and y           (Lemma 1, masking always results in a smaller unsigned number)
//   y <u y + 1 is always true if y != -1   (Lemma 2, (uint)(-1 + 1) == (uint)(UINT_MAX + 1) which overflows)
//   y <u 0 is always false for any y       (Lemma 3, 0 == UINT_MIN and nothing can be smaller than that)
//
// (1a) Always:     Change ((x & m) <=u m  ) or ((m & x) <=u m  ) to always true   (true by Lemma 1)
// (1b) If m != -1: Change ((x & m) <u  m + 1) or ((m & x) <u  m + 1) to always true:
//    x & m <=u m          is always true   // (Lemma 1)
//    x & m <=u m <u m + 1 is always true   // (Lemma 2: m <u m + 1, if m != -1)
//
// A counter example for (1b), if we allowed m == -1:
//     (x & m)  <u m + 1
//     (x & -1) <u 0
//      x       <u 0
//   which is false for any x (Lemma 3)
//
// (2) Change ((x & (m - 1)) <u m) or (((m - 1) & x) <u m) to (m >u 0)
// This is the off-by-one variant of the above.
//
// We now prove that this replacement is correct. This is the same as proving
//   "m >u 0" if and only if "x & (m - 1) <u m", i.e. "m >u 0 <=> x & (m - 1) <u m"
//
// We use (Lemma 1) and (Lemma 3) from above.
//
// Case "x & (m - 1) <u m => m >u 0":
//   We prove this by contradiction:
//     Assume m <=u 0 which is equivalent to m == 0:
//   and thus
//     x & (m - 1) <u m = 0               // m == 0
//     y           <u     0               // y = x & (m - 1)
//   by Lemma 3, this is always false, i.e. a contradiction to our assumption.
//
// Case "m >u 0 => x & (m - 1) <u m":
//   x & (m - 1) <=u (m - 1)              // (Lemma 1)
//   x & (m - 1) <=u (m - 1) <u m         // Using assumption m >u 0, no underflow of "m - 1"
//
//
// Note that the signed version of "m > 0":
//   m > 0 <=> x & (m - 1) <u m
// does not hold:
//   Assume m == -1 and x == -1:
//     x  & (m - 1) <u m
//     -1 & -2      <u -1
//     -2           <u -1
//     UINT_MAX - 1 <u UINT_MAX           // Signed to unsigned numbers
// which is true while
//   m > 0
// is false which is a contradiction.
//
// (1a) and (1b) is covered by this method since we can directly return a true value as type while (2) is covered
// in BoolNode::Ideal since we create a new non-constant node (see [CMPU_MASK]).
const Type* BoolNode::Value_cmpu_and_mask(PhaseValues* phase) const {
  Node* cmp = in(1);
  if (cmp != nullptr && cmp->Opcode() == Op_CmpU) {
    Node* cmp1 = cmp->in(1);
    Node* cmp2 = cmp->in(2);

    if (cmp1->Opcode() == Op_AndI) {
      Node* m = nullptr;
      if (_test._test == BoolTest::le) {
        // (1a) "((x & m) <=u m)", cmp2 = m
        m = cmp2;
      } else if (_test._test == BoolTest::lt && cmp2->Opcode() == Op_AddI && cmp2->in(2)->find_int_con(0) == 1) {
        // (1b) "(x & m) <u m + 1" and "(m & x) <u m + 1", cmp2 = m + 1
        Node* rhs_m = cmp2->in(1);
        const TypeInt* rhs_m_type = phase->type(rhs_m)->isa_int();
        if (rhs_m_type != nullptr && (rhs_m_type->_lo > -1 || rhs_m_type->_hi < -1)) {
          // Exclude any case where m == -1 is possible.
          m = rhs_m;
        }
      }

      if (cmp1->in(2) == m || cmp1->in(1) == m) {
        return TypeInt::ONE;
      }
    }
  }

  return nullptr;
}

// Simplify a Bool (convert condition codes to boolean (1 or 0)) node,
// based on local information.   If the input is constant, do it.
const Type* BoolNode::Value(PhaseGVN* phase) const {
  const Type* input_type = phase->type(in(1));
  if (input_type == Type::TOP) {
    return Type::TOP;
  }
  const Type* t = Value_cmpu_and_mask(phase);
  if (t != nullptr) {
    return t;
  }

  return _test.cc2logical(input_type);
}

#ifndef PRODUCT
//------------------------------dump_spec--------------------------------------
// Dump special per-node info
void BoolNode::dump_spec(outputStream *st) const {
  st->print("[");
  _test.dump_on(st);
  st->print("]");
}
#endif

//----------------------is_counted_loop_exit_test------------------------------
// Returns true if node is used by a counted loop node.
bool BoolNode::is_counted_loop_exit_test() {
  for( DUIterator_Fast imax, i = fast_outs(imax); i < imax; i++ ) {
    Node* use = fast_out(i);
    if (use->is_CountedLoopEnd()) {
      return true;
    }
  }
  return false;
}

//=============================================================================
//------------------------------Value------------------------------------------
const Type* AbsNode::Value(PhaseGVN* phase) const {
  const Type* t1 = phase->type(in(1));
  if (t1 == Type::TOP) return Type::TOP;

  switch (t1->base()) {
  case Type::Int: {
    const TypeInt* ti = t1->is_int();
    if (ti->is_con()) {
      return TypeInt::make(g_uabs(ti->get_con()));
    }
    break;
  }
  case Type::Long: {
    const TypeLong* tl = t1->is_long();
    if (tl->is_con()) {
      return TypeLong::make(g_uabs(tl->get_con()));
    }
    break;
  }
  case Type::FloatCon:
    return TypeF::make(abs(t1->getf()));
  case Type::DoubleCon:
    return TypeD::make(abs(t1->getd()));
  default:
    break;
  }

  return bottom_type();
}

//------------------------------Identity----------------------------------------
Node* AbsNode::Identity(PhaseGVN* phase) {
  Node* in1 = in(1);
  // No need to do abs for non-negative values
  if (phase->type(in1)->higher_equal(TypeInt::POS) ||
      phase->type(in1)->higher_equal(TypeLong::POS)) {
    return in1;
  }
  // Convert "abs(abs(x))" into "abs(x)"
  if (in1->Opcode() == Opcode()) {
    return in1;
  }
  return this;
}

//------------------------------Ideal------------------------------------------
Node* AbsNode::Ideal(PhaseGVN* phase, bool can_reshape) {
  Node* in1 = in(1);
  // Convert "abs(0-x)" into "abs(x)"
  if (in1->is_Sub() && phase->type(in1->in(1))->is_zero_type()) {
    set_req_X(1, in1->in(2), phase);
    return this;
  }
  return nullptr;
}

//=============================================================================
//------------------------------Value------------------------------------------
// Compute sqrt
const Type* SqrtDNode::Value(PhaseGVN* phase) const {
  const Type *t1 = phase->type( in(1) );
  if( t1 == Type::TOP ) return Type::TOP;
  if( t1->base() != Type::DoubleCon ) return Type::DOUBLE;
  double d = t1->getd();
  if( d < 0.0 ) return Type::DOUBLE;
  return TypeD::make( sqrt( d ) );
}

const Type* SqrtFNode::Value(PhaseGVN* phase) const {
  const Type *t1 = phase->type( in(1) );
  if( t1 == Type::TOP ) return Type::TOP;
  if( t1->base() != Type::FloatCon ) return Type::FLOAT;
  float f = t1->getf();
  if( f < 0.0f ) return Type::FLOAT;
  return TypeF::make( (float)sqrt( (double)f ) );
}

const Type* SqrtHFNode::Value(PhaseGVN* phase) const {
  const Type* t1 = phase->type(in(1));
  if (t1 == Type::TOP) { return Type::TOP; }
  if (t1->base() != Type::HalfFloatCon) { return Type::HALF_FLOAT; }
  float f = t1->getf();
  if (f < 0.0f) return Type::HALF_FLOAT;
  return TypeH::make((float)sqrt((double)f));
}

static const Type* reverse_bytes(int opcode, const Type* con) {
  switch (opcode) {
    // It is valid in bytecode to load any int and pass it to a method that expects a smaller type (i.e., short, char).
    // Let's cast the value to match the Java behavior.
    case Op_ReverseBytesS:  return TypeInt::make(byteswap(static_cast<jshort>(con->is_int()->get_con())));
    case Op_ReverseBytesUS: return TypeInt::make(byteswap(static_cast<jchar>(con->is_int()->get_con())));
    case Op_ReverseBytesI:  return TypeInt::make(byteswap(con->is_int()->get_con()));
    case Op_ReverseBytesL:  return TypeLong::make(byteswap(con->is_long()->get_con()));
    default: ShouldNotReachHere();
  }
}

const Type* ReverseBytesNode::Value(PhaseGVN* phase) const {
  const Type* type = phase->type(in(1));
  if (type == Type::TOP) {
    return Type::TOP;
  }
  if (type->singleton()) {
    return reverse_bytes(Opcode(), type);
  }
  return bottom_type();
}

const Type* ReverseINode::Value(PhaseGVN* phase) const {
  const Type *t1 = phase->type( in(1) );
  if (t1 == Type::TOP) {
    return Type::TOP;
  }
  const TypeInt* t1int = t1->isa_int();
  if (t1int && t1int->is_con()) {
    jint res = reverse_bits(t1int->get_con());
    return TypeInt::make(res);
  }
  return bottom_type();
}

const Type* ReverseLNode::Value(PhaseGVN* phase) const {
  const Type *t1 = phase->type( in(1) );
  if (t1 == Type::TOP) {
    return Type::TOP;
  }
  const TypeLong* t1long = t1->isa_long();
  if (t1long && t1long->is_con()) {
    jlong res = reverse_bits(t1long->get_con());
    return TypeLong::make(res);
  }
  return bottom_type();
}

Node* InvolutionNode::Identity(PhaseGVN* phase) {
  // Op ( Op x ) => x
  if (in(1)->Opcode() == Opcode()) {
    return in(1)->in(1);
  }
  return this;
}
