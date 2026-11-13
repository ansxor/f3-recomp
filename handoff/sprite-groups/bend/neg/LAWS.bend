import Base

# FALSE on purpose: the offset is added on one side only (non-rigid member).
law rigid_delta:
  for l: Nat
  for o0: Nat
  for o1: Nat
  {Nat.sub(Nat.add(l, o1), o0) == Nat.sub(o1, o0) : Nat}
