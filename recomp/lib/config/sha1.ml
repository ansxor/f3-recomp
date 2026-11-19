open Int32

let ( +! ) = add

let rotate_left word bits =
  logor (shift_left word bits) (shift_right_logical word (32 - bits))

(* SHA-1 is the chip manifest digest. Padding and message length are big-endian. *)
let hex_digest bytes =
  let length = String.length bytes in
  let tail_length = length mod 64 in
  let tail = Bytes.make (if tail_length < 56 then 64 else 128) '\x00' in
  Bytes.blit_string bytes (length - tail_length) tail 0 tail_length;
  Bytes.set tail tail_length '\x80';
  Bytes.set_int64_be tail (Bytes.length tail - 8) (Int64.mul (Int64.of_int length) 8L);
  let state = [| 0x67452301l; 0xefcdab89l; 0x98badcfel; 0x10325476l; 0xc3d2e1f0l |] in
  let words = Array.make 80 0l in
  let compress block offset =
    for i = 0 to 15 do
      words.(i) <- String.get_int32_be block (offset + 4 * i)
    done;
    for i = 16 to 79 do
      words.(i) <-
        rotate_left (logxor (logxor words.(i - 3) words.(i - 8))
                            (logxor words.(i - 14) words.(i - 16))) 1
    done;
    let a = ref state.(0) and b = ref state.(1) and c = ref state.(2)
    and d = ref state.(3) and e = ref state.(4) in
    for i = 0 to 79 do
      let parity () = logxor (logxor !b !c) !d in
      let f, k =
        if i < 20 then logor (logand !b !c) (logand (lognot !b) !d), 0x5a827999l
        else if i < 40 then parity (), 0x6ed9eba1l
        else if i < 60 then logor (logand !b !c) (logand (logor !b !c) !d), 0x8f1bbcdcl
        else parity (), 0xca62c1d6l
      in
      let next = rotate_left !a 5 +! f +! !e +! k +! words.(i) in
      e := !d;
      d := !c;
      c := rotate_left !b 30;
      b := !a;
      a := next
    done;
    Array.iteri (fun i word -> state.(i) <- state.(i) +! word)
      [| !a; !b; !c; !d; !e |]
  in
  for block = 0 to length / 64 - 1 do
    compress bytes (64 * block)
  done;
  let tail = Bytes.unsafe_to_string tail in
  for block = 0 to String.length tail / 64 - 1 do
    compress tail (64 * block)
  done;
  String.concat "" (Array.to_list (Array.map (Printf.sprintf "%08lx") state))
