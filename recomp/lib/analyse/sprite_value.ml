module Int_set = Set.Make (Int)

module Field = struct
  type t = { base : int; offset : int }
  let compare = compare
  let register_name index =
    if index < 8 then Printf.sprintf "d%d" index
    else if index < 15 then Printf.sprintf "a%d" (index - 8)
    else "sp"
  let describe { base; offset } =
    Printf.sprintf "%s+0x%02x" (register_name base) offset
end

module Fields = Set.Make (Field)

type kind =
  | Unknown | Param | Stack | Rom | Sprite | Ram | Io | Other
  | Mem | Derived | Stream | Index | Addr | Const | Data

module Tags = Set.Make (struct type t = kind let compare = compare end)

let word_mask n = n land 0xffff
let signed_word n =
  let n = word_mask n in
  if n land 0x8000 = 0 then n else n - 0x10000

let sprite_address n = 0x600000 <= n && n < 0x610000
let ram_address n = 0x400000 <= n && n < 0x600000
let address_kind ~rom_size n =
  if 0 <= n && n < rom_size then Rom
  else if sprite_address n then Sprite
  else if ram_address n then Ram
  else Io

module Word = struct
  type t = {
    direct : Fields.t;
    exact : bool;
    roots : Fields.t;
    const : int option;
    tags : Tags.t;
    transformed : bool;
  }

  let empty = {
    direct = Fields.empty; exact = true; roots = Fields.empty;
    const = None; tags = Tags.empty; transformed = false;
  }

  let field base offset =
    let fields = Fields.singleton { Field.base; offset } in
    { empty with direct = fields; roots = fields }

  let equal a b =
    a == b ||
    (a.exact = b.exact && a.const = b.const && a.transformed = b.transformed
     && Fields.equal a.direct b.direct && Fields.equal a.roots b.roots
     && Tags.equal a.tags b.tags)

  let merge a b =
    if equal a b then a
    else {
      direct = Fields.union a.direct b.direct;
      exact = a.exact && b.exact && Fields.equal a.direct b.direct;
      roots = Fields.union a.roots b.roots;
      const = (if a.const = b.const then a.const else None);
      tags = Tags.union a.tags b.tags;
      transformed = a.transformed || b.transformed;
    }

  let add ~subtract a b =
    let combine x y = word_mask (if subtract then x - y else x + y) in
    {
      direct = Fields.union a.direct b.direct;
      exact = a.exact && b.exact &&
        (Fields.is_empty a.direct || Fields.is_empty b.direct || Fields.equal a.direct b.direct);
      roots = Fields.union a.roots b.roots;
      const = (match a.const, b.const with Some x, Some y -> Some (combine x y) | _ -> None);
      tags = Tags.union a.tags b.tags;
      transformed = true;
    }

  let provenance word = Fields.union word.roots word.direct

  let derive ?(other = empty) word =
    { empty with
      exact = word.exact && other.exact;
      roots = Fields.union (provenance word) (provenance other);
      tags = Tags.add Derived (Tags.union word.tags other.tags);
      transformed = true }

  let sign_extension word =
    match word.const with
    | None ->
        { empty with exact = word.exact; tags = Tags.add Derived word.tags; transformed = true }
    | Some _ ->
        (* The reference retains the low word's bits in its extension word. *)
        { empty with const = word.const; tags = word.tags; transformed = true }

  let anchor word =
    if word.exact && Fields.cardinal word.direct = 1 then Fields.min_elt_opt word.direct
    else None

  let rigid word = word.exact && Fields.subset word.roots word.direct

  let describe_fields fields =
    match Fields.elements fields with
    | [] -> "none"
    | fields -> String.concat ", " (List.map Field.describe fields)

  let unresolved_reason word =
    if Fields.cardinal word.direct > 1 then "ambiguous root fields " ^ describe_fields word.direct
    else if not word.exact then "unproven value (merged or unknown provenance)"
    else if Tags.mem Stream word.tags then "value read from a stream cursor (no record field)"
    else if Option.is_some word.const then "constant value"
    else if not (Fields.is_empty word.roots) then "no direct root among " ^ describe_fields word.roots
    else "value has no record field"
end

module Value = struct
  type t = { hi : Word.t; lo : Word.t; cursor : bool; offsets : Int_set.t; kind : kind }

  let words ?(hi = Word.empty) ?(lo = Word.empty) kind =
    { hi; lo; cursor = false; offsets = Int_set.empty; kind }

  let uniform kind word = words ~hi:word ~lo:word kind
  let unknown kind = uniform kind { Word.empty with exact = false; tags = Tags.singleton kind }
  let constant ~rom_size value =
    let value = Insn.address value in
    let kind = address_kind ~rom_size value in
    let word n = { Word.empty with const = Some (word_mask n); tags = Tags.singleton kind } in
    words ~hi:(word (value lsr 16)) ~lo:(word value) kind

  let const32 value =
    match value.hi.const, value.lo.const with
    | Some hi, Some lo -> Some ((hi lsl 16) lor lo)
    | _ -> None

  let join_kind a b =
    if a = b || b = Unknown then a else if a = Unknown then b else Other

  let equal a b =
    a == b ||
    (a.cursor = b.cursor && a.kind = b.kind && Int_set.equal a.offsets b.offsets
     && Word.equal a.hi b.hi && Word.equal a.lo b.lo)

  let merge a b =
    if equal a b then a
    else {
      hi = Word.merge a.hi b.hi; lo = Word.merge a.lo b.lo;
      cursor = a.cursor || b.cursor; offsets = Int_set.union a.offsets b.offsets;
      kind = join_kind a.kind b.kind;
    }

  let provenance value = Fields.union (Word.provenance value.hi) (Word.provenance value.lo)
  let tags value = Tags.union value.hi.tags value.lo.tags
  let without_cursor value = { value with cursor = false; offsets = Int_set.empty }
  let cursor_offsets value offsets = { value with cursor = not (Int_set.is_empty offsets); offsets }
  let at_address value address =
    if sprite_address address then cursor_offsets value (Int_set.singleton (address land 15))
    else without_cursor value

  let bump value delta =
    if value.cursor then { value with offsets = Int_set.map (fun offset -> (offset + delta) land 15) value.offsets }
    else value
end

module State = struct
  (* Arrays are shared between work-list entries and copied before writing. *)
  type t = Value.t array

  let sp = 15
  let work = 13
  let register = function Insn.D n -> n | Insn.A n -> n + 8
  let get (state : t) index = state.(index)
  let default work_base =
    let state = Array.make 16 (Value.unknown Param) in
    state.(sp) <- Value.unknown Stack;
    Option.iter (fun value -> state.(work) <- value) work_base;
    state

  let update state index value =
    if Value.equal state.(index) value then state
    else (
      let updated = Array.copy state in
      updated.(index) <- value;
      updated)

  let merge previous incoming =
    let merged = Array.mapi (fun index value -> Value.merge value incoming.(index)) previous in
    if Array.for_all2 Value.equal previous merged then previous else merged
end
