open F3recomp
open Json

let json = Alcotest.testable (fun ppf value -> Stdlib.Format.pp_print_string ppf (to_string value)) ( = )
let text = Alcotest.(check string)

let serialize () =
  text "escapes" {|"a\"b\\c\n\r\t\b\f\u0001\u001f é"|}
    (Json.to_string (String "a\"b\\c\n\r\t\b\012\001\031 \xc3\xa9"));
  text "scalars" "[null,true,false,-7,9223372036854775807,1.5,3,null]"
    (Json.to_string (Array [Null; Bool true; Bool false; Int (-7); Int64 Int64.max_int;
                            Float 1.5; Float 3.; Float Float.nan]));
  text "nested" {|{"a":[1,{"b":[]},{}],"c":"d"}|}
    (Json.to_string (Object ["a", Array [Int 1; Object ["b", Array []]; Object []]; "c", String "d"]));
  text "keys are escaped" {|{"a\"b":0}|} (Json.to_string (Object ["a\"b", Int 0]));
  text "hex" {|"0x00400a"|} (Json.to_string (Json.hex 0x400a))

let parse_ok source = match Json.parse source with
  | Ok value -> value
  | Error message -> Alcotest.failf "%s: %s" source message

let parse_values () =
  Alcotest.check json "whitespace" (Array [Int 1; Int 2]) (parse_ok " \n[ 1 ,\t2 ]\r\n");
  Alcotest.check json "numbers"
    (Array [Int 0; Int (-12); Int 3000; Float 1.5; Float (-0.25); Float 1e3; Int64 Int64.max_int;
            Int64 Int64.min_int])
    (parse_ok "[0,-12,3000,1.5,-0.25,1E3,9223372036854775807,-9223372036854775808]");
  Alcotest.check json "beyond int64" (Float 1.8446744073709552e19) (parse_ok "18446744073709551616");
  Alcotest.check json "literals" (Array [Null; Bool true; Bool false]) (parse_ok "[null,true,false]");
  Alcotest.check json "nested"
    (Object ["a", Array [Object ["b", Null]]; "c", Object []])
    (parse_ok {|{"a":[{"b":null}],"c":{}}|});
  Alcotest.check json "duplicate keys keep order" (Object ["a", Int 1; "a", Int 2])
    (parse_ok {|{"a":1,"a":2}|})

let parse_strings () =
  Alcotest.check json "simple escapes" (String "\"\\/\b\012\n\r\t") (parse_ok {|"\"\\\/\b\f\n\r\t"|});
  Alcotest.check json "BMP escape" (String "\xc3\xa9\xe2\x82\xac") (parse_ok {|"\u00e9\u20AC"|});
  Alcotest.check json "surrogate pair" (String "\xf0\x9f\x98\x80") (parse_ok {|"\ud83d\ude00"|});
  Alcotest.check json "raw UTF-8 passes through" (String "\xc3\xa9") (parse_ok "\"\xc3\xa9\"")

let rejects source =
  match Json.parse source with
  | Ok value -> Alcotest.failf "%S parsed as %s" source (to_string value)
  | Error _ -> ()

let errors () =
  List.iter rejects
    [ ""; " "; "{"; "[1,]"; "[1 2]"; "{\"a\" 1}"; "{\"a\":}"; "{a:1}"; "tru"; "nul"; "\"abc";
      "\"\\x\""; "\"\\u12\""; "\"\\ud83d\""; "\"\\ude00\""; "\"a\nb\""; "1 2"; "01"; "-"; "1."; "1e";
      "[1]]"; "'a'"; "{\"a\":1,}" ];
  (match Json.parse "[1,x]" with
   | Error message ->
       Alcotest.(check bool) "offset reported" true (String.length message > 0
         && (try ignore (Str.search_forward (Str.regexp_string "offset 3") message 0); true
             with Not_found -> false))
   | Ok _ -> Alcotest.fail "expected a syntax error");
  rejects (String.concat "" (List.init 600 (fun _ -> "[")))

let round_trip () =
  let values = [
    Null; Bool true; Int 0; Int (-1); Int 1_000_000_000_000; Int64 Int64.max_int; Float 0.5;
    Float (-1234.5678); String ""; String "tab\t\"quote\" \\ slash \x01 \xc3\xa9 \xf0\x9f\x98\x80";
    Array []; Object [];
    Object ["rows", Array [Object ["pc", Int 0x400; "text", String "move.l d0,(a0)+"];
                           Object ["pc", Int 0x402; "ok", Bool false]];
            "empty", Null; "nested", Array [Array [Array []]]];
  ] in
  List.iter (fun value -> Alcotest.check json (to_string value) value (parse_ok (to_string value)))
    values

let member () =
  let value = Object ["a", Int 1; "b", Null] in
  Alcotest.(check (option json)) "present" (Some (Int 1)) (Json.member "a" value);
  Alcotest.(check (option json)) "null field" (Some Null) (Json.member "b" value);
  Alcotest.(check (option json)) "missing" None (Json.member "c" value);
  Alcotest.(check (option json)) "not an object" None (Json.member "a" (Array []))

let () =
  Alcotest.run "json" [
    "json", [
      Alcotest.test_case "serialize" `Quick serialize;
      Alcotest.test_case "parse values" `Quick parse_values;
      Alcotest.test_case "parse strings" `Quick parse_strings;
      Alcotest.test_case "syntax errors" `Quick errors;
      Alcotest.test_case "round trip" `Quick round_trip;
      Alcotest.test_case "member" `Quick member;
    ]]
