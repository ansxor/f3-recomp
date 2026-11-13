import Base

# Sprite-group identity and rigid interpolation, for all values (unbounded).
#
# A rigid group has origin O; member S sits at pos(S) = l(S) + O with a template
# offset l that does not depend on O. Positions are Nat; motion O0 -> O1 with
# O1 >= O0 has delta O1 - O0 (the other direction is the same law, swapped).

type Cls is Data:
  Body{}
  Shadow{}

# Every member of a rigid group moves by exactly the group's delta, whatever its
# template offset: pairing the group alone interpolates every member correctly.
law rigid_delta:
  for l: Nat
  for o0: Nat
  for o1: Nat
  {Nat.sub(Nat.add(l, o1), Nat.add(l, o0)) == Nat.sub(o1, o0) : Nat}

# A birth event bumps the generation, so a new incarnation never shares the
# previous incarnation's key (in-place replacement cannot alias).
law birth_separates:
  for g: Nat
  {1n+g != g : Nat}

# The class component keeps an object's shadow group apart from its body group
# (the shadow has its own origin, $44 in commandw).
law class_separates:
  {Body{} != Shadow{} : Cls}
