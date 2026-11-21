type t = { base : int; bytes : string }

let length image = String.length image.bytes

let contains_range image address ~length:count =
  count >= 0 && count <= length image && address >= image.base
  && (let offset = address - image.base in offset >= 0 && offset <= length image - count)

let contains image address = contains_range image address ~length:1

let offset image address ~length =
  if not (contains_range image address ~length) then
    Error.fail ~where:"image" "read of %d bytes at %#x outside image" length address;
  address - image.base

let u8 image address = String.get_uint8 image.bytes (offset image address ~length:1)
let s8 image address = String.get_int8 image.bytes (offset image address ~length:1)
let u16 image address = String.get_uint16_be image.bytes (offset image address ~length:2)
let s16 image address = String.get_int16_be image.bytes (offset image address ~length:2)
let s32 image address =
  Int32.to_int (String.get_int32_be image.bytes (offset image address ~length:4))

let u32 image address = s32 image address land 0xffffffff

let slice image address ~length =
  { base = address; bytes = String.sub image.bytes (offset image address ~length) length }

let crc32 image =
  let module Crc = Checkseum.Crc32 in
  Crc.digest_string image.bytes 0 (length image) Crc.default |> Crc.to_int32
