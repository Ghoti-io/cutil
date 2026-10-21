# Base64

`base64.h` encodes and decodes RFC 4648 base64 (sections 4 and 5) in buffers the caller owns. It allocates nothing,
holds no state, reads no locale, and gives the same bytes on every target.

```c
#include <ghoti.io/cutil/base64.h>

char text[64];
size_t n;
gcu_base64_encode("foobar", 6, text, sizeof text,
    GCU_BASE64_ALPHABET_STANDARD, GCU_BASE64_ENCODE_PADDING_ALWAYS, &n);      /* "Zm9vYmFy", n == 8 */

unsigned char bytes[16];
size_t bad;
GCU_Base64_Status s = gcu_base64_decode(text, n, bytes, sizeof bytes, NULL, &n, &bad);
```

## Strictness is a choice you make at the call

Two decoders can disagree about one string. So every choice is a field of `GCU_Base64_Decode_Options`, and passing
`NULL` (or a zero-filled struct) selects the strictest reading, RFC 4648 section 3.5's canonical form:

| Field | Zero value | Other values |
| --- | --- | --- |
| `alphabet` | standard (`+` `/`) | URL (`-` `_`); the other alphabet's two characters are errors |
| `padding` | required | optional (accepted when complete, may be left out); forbidden |
| `whitespace` | rejected | skip ASCII (space, tab, CR, LF, FF), never inside an unfinished padding |
| `trailing_bits` | the spare bits of the last character must be zero | ignored |

`=` is accepted only as the last one or two characters and only to complete a group of four; data after padding is an
error, and so is a final group of one character, which carries no whole byte. A `data:` URI wants whitespace
skipping and optional padding, and says so:

```c
GCU_Base64_Decode_Options o = {GCU_BASE64_ALPHABET_STANDARD, GCU_BASE64_PADDING_OPTIONAL,
    GCU_BASE64_WHITESPACE_SKIP_ASCII, GCU_BASE64_TRAILING_BITS_MUST_BE_ZERO};
```

## Results and sizes

`gcu_base64_decode` checks the whole input before it writes anything, so a caller can never mistake a partial result
for a whole one. On an error it reports the offset of the first byte refused (`n` for an error at the end of the
input). Pass `out == NULL` to measure. When the buffer is too small the result is `GCU_BASE64_ERR_CAPACITY` and the
size needed is in `written`. Decoding may be done in place (`out == in`); encoding may not.

`gcu_base64_encoded_size` is exact and reports overflow instead of wrapping; `gcu_base64_decoded_size_max` is an
upper bound that cannot overflow.

## Testing

`test/test-base64.cpp` runs RFC 4648's vectors, a matrix of malformed inputs with their expected statuses and
offsets, the overflow boundaries, 20,000 random buffers against a bit-at-a-time reference encoder written for the
test, and `test/data/base64-vectors.txt`, 224 encodings made by Python's `base64` module
(`tools/gen-base64-vectors.py` writes the file and `--check` verifies it). `make check-base64-defects` plants sixteen
ways the codec can be wrong (data after padding accepted, the other alphabet accepted, trailing bits ignored,
whitespace inside padding, a wrapped size, ...) and requires the tests to notice each one.
