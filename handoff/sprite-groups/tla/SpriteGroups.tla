---------------------------- MODULE SpriteGroups ----------------------------
(* Sprite identity across frames for one emit unit (commandw `objects`).
   Ground truth: each object incarnation has a ghost `uid`; each drawn sprite
   has a ghost `role` (shadow, or body tile i of LoD level l).
   The runtime only sees: object address, a detected generation, and the
   order of entries inside the object's span.

   Frame model (commandw): per object, shadow entries are emitted first and
   only on odd counter parity; then the body tiles of the current LoD level.
   Objects may die, be born, be replaced in place, change level, reorder. *)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    Addr,        \* object slot addresses, e.g. {1, 2}
    Levels,      \* LoD levels, e.g. {0, 1}; level 0 = full detail
    Shadowed,    \* addresses whose object has a shadow
    MaxUid,      \* bound on incarnations (state constraint)
    Scheme,      \* "order" (current: object id + slot - lo) | "role" (proposed)
    Birth        \* "gap" (current: new generation after a missing frame) | "event"

\* Body tiles per level: full detail draws more tiles (commandw L0 10x12 .. L3 2x2).
Tiles == [l \in Levels |-> IF l = 0 THEN 2 ELSE 1]

VARIABLES live, uid, nextUid, gen, lvl, parity, order, prev

vars == <<live, uid, nextUid, gen, lvl, parity, order, prev>>

Perms(S) == { f \in [1..Cardinality(S) -> S] :
                \A i, j \in 1..Cardinality(S) : i # j => f[i] # f[j] }

Roles(a, l, p) ==
    (IF a \in Shadowed /\ p THEN << <<"shadow", 0, 0>> >> ELSE << >>)
    \o [i \in 1..Tiles[l] |-> <<"body", l, i>>]

\* Sprites drawn by a state: one record per entry; k = offset inside the object span.
Frame(lv, u, g, l, p, ord) ==
    UNION { { [uid |-> u[ord[n]], addr |-> ord[n], gen |-> g[ord[n]],
               role |-> Roles(ord[n], l[ord[n]], p)[k], k |-> k]
              : k \in 1..Len(Roles(ord[n], l[ord[n]], p)) }
            : n \in 1..Len(ord) }

Cur == Frame(live, uid, gen, lvl, parity, order)

GroupKey(s) == <<s.addr, s.gen>>
Id(s) == IF Scheme = "role" THEN <<s.addr, s.gen, s.role>> ELSE <<s.addr, s.gen, s.k>>

Init ==
    /\ live \in [Addr -> BOOLEAN]
    /\ uid = [a \in Addr |-> a]
    /\ nextUid = Cardinality(Addr) + 1
    /\ gen = [a \in Addr |-> 0]
    /\ lvl \in [Addr -> Levels]
    /\ parity \in BOOLEAN
    /\ order \in Perms({a \in Addr : live[a]})
    /\ prev = {}

\* fate[a]: "dead", "same" (same incarnation; only if live), "new" (fresh incarnation,
\* possibly reusing a live slot in the same frame).
Next ==
    \E fate \in [Addr -> {"dead", "same", "new"}] :
        /\ \A a \in Addr : fate[a] = "same" => live[a]
        /\ live'    = [a \in Addr |-> fate[a] # "dead"]
        /\ uid'     = [a \in Addr |-> IF fate[a] = "new" THEN nextUid + a ELSE uid[a]]
        /\ nextUid' = nextUid + Cardinality(Addr) + 1
        /\ gen'     = [a \in Addr |->
                        IF Birth = "event"
                        THEN IF fate[a] = "new" THEN gen[a] + 1 ELSE gen[a]
                        ELSE IF fate[a] # "dead" /\ ~live[a] THEN gen[a] + 1 ELSE gen[a]]
        /\ lvl'    \in [Addr -> Levels]
        /\ parity' = ~parity
        /\ order'  \in Perms({a \in Addr : fate[a] # "dead"})
        /\ prev'   = Cur

Spec == Init /\ [][Next]_vars

Bound == nextUid <= MaxUid

\* ---- Properties over consecutive frames (prev = frame t-1, Cur = frame t) ----

\* No two sprites in one frame share an id.
Unique == \A s, t \in Cur : Id(s) = Id(t) => s = t

\* SG key: equal keys only for the same incarnation ...
GroupSound    == \A s \in prev, t \in Cur : GroupKey(s) = GroupKey(t) => s.uid = t.uid
\* ... and the same incarnation always keeps its key (through LoD and flicker).
GroupComplete == \A s \in prev, t \in Cur : s.uid = t.uid => GroupKey(s) = GroupKey(t)

\* Sprite id pairs only the same sprite (same incarnation, same role) ...
PairSound    == \A s \in prev, t \in Cur : Id(s) = Id(t) => s.uid = t.uid /\ s.role = t.role
\* ... and a sprite visible in both frames keeps its id.
PairComplete == \A s \in prev, t \in Cur : (s.uid = t.uid /\ s.role = t.role) => Id(s) = Id(t)
=============================================================================
