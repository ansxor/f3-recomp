import Base
import ./LAWS.bend as Laws

def zero_disc(n: Nat) -> Type:
  match n:
    case 0n:
      Empty
    case 1n+p:
      Unit

def pred(n: Nat) -> Nat:
  match n:
    case 0n:
      0n
    case 1n+p:
      p

def cls_disc(c: Laws.Cls) -> Type:
  match c:
    case Laws.Body{}:
      Unit
    case Laws.Shadow{}:
      Empty

# Induction on l: both sides gain one successor, which Nat.sub cancels.
def Laws.rigid_delta(l, o0, o1):
  match l:
    case 0n:
      {==}
    case 1n+p:
      Laws.rigid_delta(p, o0, o1)

# 0: 1 == 0 is refuted by the zero discriminator. Step: strip one successor from
# both sides and use the induction hypothesis.
def Laws.birth_separates(g):
  match g:
    case 0n:
      e => %e : zero_disc(_); Unit{}
    case 1n+p:
      e => Laws.birth_separates(p, Equal.cong(Nat, Nat, pred, 1n+1n+p, 1n+p, e))

def Laws.class_separates():
  e => %e : cls_disc(_); Unit{}
