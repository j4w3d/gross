/*
 * Copyright (c) 2006 Antti Siira <antti@utu.fi>
 *                    Eino Tuominen <eino@utu.fi>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "srvutils.h"
#include "sha256.h"

void (*p_convert_int64_to_big_endian)(sha_ulong_t *num) = &convert_int64_big_endian;
void (*p_convert_int32_to_big_endian)(sha_uint_t *num) = &convert_int32_big_endian;

const sha_256_t DEFAULT_SHA256 = { //232 times the square root of the first 8 primes 2..19
  0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
				
unsigned int ROUND_CONSTANTS[64] = { //232 times the cube root of the first 64 primes 2..311
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

// Rotate bits right eg. right 1 bit == 10100100 => 01010010
sha_uint_t rotate_right(sha_uint_t num, int amount) {
  return (num >> amount) | (num << (32 - amount));
}

// Return TRUE if little endian. False othervice.
int little_endian() {
  int i = 1;
  char *p = (char *) &i;
  if (p[0] == 1) // Lowest address contains the least significant byte
    return TRUE;
  else
    return FALSE;
}

void swap_bytes(sha_byte_t* a, sha_byte_t* b) {
  sha_byte_t tmp = *a;
  *a = *b;
  *b = tmp;
}

void convert_int64_little_endian (sha_ulong_t *num) {
  sha_byte_t* index = (sha_byte_t*)num;

  swap_bytes(index+0, index+7);
  swap_bytes(index+1, index+6);
  swap_bytes(index+2, index+5);
  swap_bytes(index+3, index+4);
}

void convert_int64_big_endian (sha_ulong_t *num) {
  do { num = num; } while (0);
}

void convert_int32_little_endian (sha_uint_t *num) {
  sha_byte_t* index = (sha_byte_t*)num;

  swap_bytes(index+0, index+3);
  swap_bytes(index+1, index+2);
}

void convert_int32_big_endian (sha_uint_t *num) {
  do { num = num; } while (0);
}

void debug_print_digest(sha_256_t digest, int with_newline) {
  printf("sha256: %08x %08x %08x %08x %08x %08x %08x %08x", digest.h0, digest.h1, digest.h2, digest.h3, digest.h4, digest.h5, digest.h6, digest.h7);
  if ( with_newline ) printf("\n");
}

sha_256_t sha256(sha_byte_t* message, sha_ulong_t size) {
  sha_256_t digest = DEFAULT_SHA256;
  sha_256_t tmp_digest = DEFAULT_SHA256;

  sha_ulong_t new_size = size + 1 + 8 + ((448 - (((sha_ulong_t) 8) * (size + 1))) % 512) / 8;
  sha_ulong_t tmp_size = size * 8;
  sha_uint_t i, j; // Multipurpose indexes
  sha_byte_t* iter;
  sha_uint_t w[64];
  sha_uint_t s0, s1, maj, t2, ch, t1, chunk;
  
  sha_byte_t* digestable_message = (sha_byte_t *)Malloc(sizeof(sha_byte_t)*new_size);

  if ( little_endian() ) {
    p_convert_int64_to_big_endian = &convert_int64_little_endian;
    p_convert_int32_to_big_endian = &convert_int32_little_endian;
  }

  // Initialize buffer
  bzero(digestable_message, new_size);
  memcpy((void*)digestable_message, (const void*)message, size);
  digestable_message[size] = 0x80;
  
  // Insert message bit length to the end of the buffer in big endian 64 integer
  (*p_convert_int64_to_big_endian)(&tmp_size);
  for ( i=new_size - 8, j=0, iter=(sha_byte_t*)&tmp_size ; i<new_size ; i++, j++ ) {
    *(digestable_message+i) = *(iter+j);
  }

  iter = (sha_byte_t*)digestable_message;

  
  for ( i = 0 ; i < new_size ; i += 64 ) {

    // Initialize the beginning 0..15 of the word block
    for ( j = 0 ; j < 64 ; j += 4 ) {
      chunk = *((sha_uint_t*)(iter+i+j));
      p_convert_int32_to_big_endian(&chunk);
      w[j/4] = chunk;
    }

    // Initialize the end 16..63 of the word block
    for (j = 16 ; j < 64 ; j++) {
      s0 = rotate_right(w[j-15], 7) ^ rotate_right(w[j-15], 18) ^ (w[j-15] >> 3);
      s1 = rotate_right(w[j-2], 17) ^ rotate_right(w[j-2], 19) ^ (w[j-2] >> 10);
      w[j] = w[j-16] + s0 + w[j-7] + s1;
    }

    // Init the temporary digest
    tmp_digest = digest;

    // Main loop
    for ( j=0 ; j<64 ; j++ ) {
      s0 = rotate_right(tmp_digest.h0, 2) ^ rotate_right(tmp_digest.h0, 13) ^ rotate_right(tmp_digest.h0, 22);
      maj = (tmp_digest.h0 & tmp_digest.h1) ^ (tmp_digest.h1 & tmp_digest.h2) ^ (tmp_digest.h2 & tmp_digest.h0);      
      t2 = s0 + maj;
      s1 = rotate_right(tmp_digest.h4, 6) ^ rotate_right(tmp_digest.h4, 11) ^ rotate_right(tmp_digest.h4, 25);
      ch = (tmp_digest.h4 & tmp_digest.h5) ^ ((~tmp_digest.h4) & tmp_digest.h6);      
      t1 = tmp_digest.h7 + s1 + ch + ROUND_CONSTANTS[j] + w[j];

      // Update values for next iteration
      tmp_digest.h7 = tmp_digest.h6;
      tmp_digest.h6 = tmp_digest.h5;
      tmp_digest.h5 = tmp_digest.h4;
      tmp_digest.h4 = tmp_digest.h3 + t1;
      tmp_digest.h3 = tmp_digest.h2;
      tmp_digest.h2 = tmp_digest.h1;
      tmp_digest.h1 = tmp_digest.h0;
      tmp_digest.h0 = t2 + t1;
    }
	
    //Add this chunk's hash to result so far:
    digest.h0 += tmp_digest.h0;
    digest.h1 += tmp_digest.h1;
    digest.h2 += tmp_digest.h2;
    digest.h3 += tmp_digest.h3;
    digest.h4 += tmp_digest.h4;
    digest.h5 += tmp_digest.h5;
    digest.h6 += tmp_digest.h6;
    digest.h7 += tmp_digest.h7;

  }

  // Free Willy!
  free(digestable_message);

  return digest;
}

sha_256_t sha256_string(char* message) {
  return sha256(((sha_byte_t*)message), strlen(message));
}

