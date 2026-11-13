import Base
import ./LAWS.bend as Laws

def Laws.rigid_delta(l, o0, o1):
  match l:
    case 0n:
      {==}
    case 1n+p:
      Laws.rigid_delta(p, o0, o1)
