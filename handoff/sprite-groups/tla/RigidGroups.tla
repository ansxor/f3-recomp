----------------------------- MODULE RigidGroups -----------------------------
(* Group-only interpolation for rigid sprite groups (commandw `objects`,
   landmakrj display objects). No per-sprite roles or slot order.

   Each object incarnation (ghost `uid`) draws a body group and, if shadowed,
   a shadow group. Each group has its own origin O (body: $6/$8, shadow: $44 in
   commandw); the shadow is a separate projection, so its O moves independently
   of the body's. Members sit at template offsets that depend only on the
   current LoD level: pos(S) = O + l_level(S). Members may appear/vanish with
   the level. The shadow is drawn on odd parity (flicker), on every frame
   (forced), or on every frame after one that drew it (fill).

   Renderer rule under test: pair groups by key (under "snap", only keys that
   name one owner per frame); interpolate each group's O; draw the current
   frame's members at lerp(O) + l_t(S). *)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Addr,       \* object slot addresses, e.g. {1, 2}
    Levels,     \* LoD levels, e.g. {0, 1}
    Pos,        \* origin values, e.g. 0..2
    Shadowed,   \* addresses whose object has a shadow
    MaxUid,     \* bound on incarnations (state constraint)
    Grouping,   \* "object": key (addr, gen) | "class": key (addr, gen, class)
    ShadowMode, \* "flicker": odd parity | "forced": every frame
                \* | "fill": odd parity, or after a frame that drew it
    Unresolved  \* "pair": pair on key match
                \* | "snap": a key pairs only if it names one owner in both frames

ASSUME ShadowMode \in {"flicker", "forced", "fill"}
ASSUME Unresolved \in {"pair", "snap"}

\* Body members per level: full detail (0) draws more tiles.
Members == [l \in Levels |-> IF l = 0 THEN 2 ELSE 1]

\* drawnBefore[a]: a shadow of the current incarnation at a was drawn in an earlier frame.
VARIABLES live, uid, nextUid, gen, lvl, parity, bodyO, shadowO, prev, drawnBefore

vars == <<live, uid, nextUid, gen, lvl, parity, bodyO, shadowO, prev, drawnBefore>>

ShadowDrawn(a, p, pv, u) ==
    a \in Shadowed /\ (ShadowMode = "forced" \/ p
                        \/ (ShadowMode = "fill" /\ (\E s \in pv : s.cls = "shadow" /\ s.uid = u[a])))

\* One record per drawn sprite. `o` is the origin of the group the sprite truly
\* belongs to; `cls` and `uid` are ground truth. `pv` is the previous frame's
\* records: the fill mode's cache of the last drawn shadow.
Frame(lv, u, g, l, p, bo, so, pv) ==
    UNION { { [uid |-> u[a], addr |-> a, gen |-> g[a], cls |-> "body", o |-> bo[a], m |-> m]
              : m \in 1..Members[l[a]] }
            : a \in {x \in Addr : lv[x]} }
    \cup
    { [uid |-> u[a], addr |-> a, gen |-> g[a], cls |-> "shadow", o |-> so[a], m |-> 1]
        : a \in {x \in Addr : lv[x] /\ ShadowDrawn(x, p, pv, u)} }

Cur == Frame(live, uid, gen, lvl, parity, bodyO, shadowO, prev)

Key(s) == IF Grouping = "class" THEN <<s.addr, s.gen, s.cls>> ELSE <<s.addr, s.gen>>

\* Renderer pairing. "pair": same key pairs. "snap": a key pairs only when it names
\* one owner (uid, cls) in this frame and in the previous frame.
Owners(S, k) == { <<s.uid, s.cls>> : s \in {x \in S : Key(x) = k} }
Resolved(k) == Cardinality(Owners(Cur, k)) = 1 /\ Cardinality(Owners(prev, k)) = 1
Pairs(s, t) == Key(s) = Key(t) /\ (Unresolved = "pair" \/ Resolved(Key(t)))

Init ==
    /\ live \in [Addr -> BOOLEAN]
    /\ uid = [a \in Addr |-> a]
    /\ nextUid = Cardinality(Addr) + 1
    /\ gen = [a \in Addr |-> 0]
    /\ lvl \in [Addr -> Levels]
    /\ parity \in BOOLEAN
    /\ bodyO \in [Addr -> Pos]
    /\ shadowO \in [Addr -> Pos]
    /\ prev = {}
    /\ drawnBefore = [a \in Addr |-> FALSE]

\* Birth is an event (direct birth signal); the gap rule was refuted by SpriteGroups.
Next ==
    \E fate \in [Addr -> {"dead", "same", "new"}] :
        /\ \A a \in Addr : fate[a] = "same" => live[a]
        /\ live'    = [a \in Addr |-> fate[a] # "dead"]
        /\ uid'     = [a \in Addr |-> IF fate[a] = "new" THEN nextUid + a ELSE uid[a]]
        /\ nextUid' = nextUid + Cardinality(Addr) + 1
        /\ gen'     = [a \in Addr |-> IF fate[a] = "new" THEN gen[a] + 1 ELSE gen[a]]
        /\ lvl'     \in [Addr -> Levels]
        /\ parity'  = ~parity
        /\ bodyO'   \in [Addr -> Pos]          \* body and shadow origins move
        /\ shadowO' \in [Addr -> Pos]          \* independently every frame
        /\ prev'    = Cur
        /\ drawnBefore' = [a \in Addr |-> fate[a] = "same" /\ (drawnBefore[a] \/ \E t \in Cur : t.cls = "shadow" /\ t.addr = a)]

Spec == Init /\ [][Next]_vars

Bound == nextUid <= MaxUid

\* ---- Properties (prev = frame t-1, Cur = frame t) ----

\* A group has one origin: one delta moves every member correctly.
RigidKey == \A s, t \in Cur : Key(s) = Key(t) => s.o = t.o

\* Equal keys across frames only for the same incarnation and the same group.
GroupSound == \A s \in prev, t \in Cur :
                Key(s) = Key(t) => s.uid = t.uid /\ s.cls = t.cls

\* The same group keeps its key (through LoD changes and member-set changes).
GroupComplete == \A s \in prev, t \in Cur :
                   s.uid = t.uid /\ s.cls = t.cls => Key(s) = Key(t)

\* Interpolation correctness: for each current sprite the renderer pairs with a
\* previous sprite, that previous sprite's origin equals the origin of every
\* previous member of the sprite's own group (so one delta fits the whole group).
Interpolates == \A t \in Cur, s \in prev :
                  Pairs(s, t) =>
                    \A s2 \in prev : (s2.uid = t.uid /\ s2.cls = t.cls) => s.o = s2.o

\* Once a shadow has been drawn for an incarnation, every later frame of that
\* incarnation pairs it with a previous-frame record. Frames before the first draw
\* are exempt: a shadow can first appear on a parity-on frame after its object's birth.
ShadowSmooth == \A t \in Cur :
                  (t.cls = "shadow" /\ drawnBefore[t.addr]) =>
                    \E s \in prev : Pairs(s, t)

\* No wrong pairing: a pair the renderer forms joins one group of one object.
PairSound == \A s \in prev, t \in Cur : Pairs(s, t) => s.uid = t.uid /\ s.cls = t.cls
=============================================================================
