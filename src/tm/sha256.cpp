/*
tm-obs-director — VEX Tournament Manager automation for OBS Studio
Copyright (C) 2026 UNSW Robotics Competition Club <kevin.li@sydneyrobotics.com.au>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "tm/sha256.hpp"

#include <cstring>
#include <vector>

namespace tmd {

namespace {

/// The first thirty-two bits of the fractional parts of the cube roots of the
/// first sixty-four primes. FIPS 180-4, section 4.2.2.
constexpr uint32_t kRoundConstants[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/// The first thirty-two bits of the fractional parts of the square roots of the
/// first eight primes. FIPS 180-4, section 5.3.3.
constexpr uint32_t kInitialState[8] = {
	0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

uint32_t rotateRight(uint32_t value, int bits)
{
	return (value >> bits) | (value << (32 - bits));
}

/// One 64-byte block, FIPS 180-4 section 6.2.2.
void compress(uint32_t state[8], const uint8_t block[kSha256BlockSize])
{
	uint32_t w[64];

	// Big-endian, which is what the spec says and not what any machine this runs
	// on stores natively, so the bytes are assembled explicitly rather than cast.
	for (int i = 0; i < 16; ++i) {
		w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
		       (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
	}

	for (int i = 16; i < 64; ++i) {
		const uint32_t s0 = rotateRight(w[i - 15], 7) ^ rotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const uint32_t s1 = rotateRight(w[i - 2], 17) ^ rotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	uint32_t a = state[0];
	uint32_t b = state[1];
	uint32_t c = state[2];
	uint32_t d = state[3];
	uint32_t e = state[4];
	uint32_t f = state[5];
	uint32_t g = state[6];
	uint32_t h = state[7];

	for (int i = 0; i < 64; ++i) {
		const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
		const uint32_t ch = (e & f) ^ (~e & g);
		const uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
		const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
		const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		const uint32_t temp2 = s0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

} // namespace

Sha256Digest sha256(const uint8_t *data, size_t length)
{
	uint32_t state[8];
	std::memcpy(state, kInitialState, sizeof(state));

	size_t offset = 0;
	for (; offset + kSha256BlockSize <= length; offset += kSha256BlockSize)
		compress(state, data + offset);

	// The tail: what is left, a 0x80 byte, zero padding, and the message length
	// in bits as a big-endian 64-bit value. Two blocks when the remainder leaves
	// no room for the length, one otherwise.
	uint8_t tail[kSha256BlockSize * 2] = {0};
	const size_t remaining = length - offset;
	// Guarded because memcpy from a null pointer is undefined even for zero
	// bytes, and hashing an empty string is a case the vectors cover.
	if (remaining > 0)
		std::memcpy(tail, data + offset, remaining);
	tail[remaining] = 0x80;

	const size_t tailBlocks = (remaining + 1 + 8 > kSha256BlockSize) ? 2 : 1;
	const size_t tailSize = tailBlocks * kSha256BlockSize;

	const uint64_t bits = static_cast<uint64_t>(length) * 8;
	for (int i = 0; i < 8; ++i)
		tail[tailSize - 1 - i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xff);

	for (size_t i = 0; i < tailBlocks; ++i)
		compress(state, tail + i * kSha256BlockSize);

	Sha256Digest digest{};
	for (int i = 0; i < 8; ++i) {
		digest[i * 4] = static_cast<uint8_t>((state[i] >> 24) & 0xff);
		digest[i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16) & 0xff);
		digest[i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8) & 0xff);
		digest[i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xff);
	}
	return digest;
}

Sha256Digest hmacSha256(const uint8_t *key, size_t keyLength, const uint8_t *message, size_t messageLength)
{
	// RFC 2104: a key longer than the block size is replaced by its own hash,
	// and a shorter one is zero-padded out to the block size.
	uint8_t paddedKey[kSha256BlockSize] = {0};
	if (keyLength > kSha256BlockSize) {
		const Sha256Digest hashed = sha256(key, keyLength);
		std::memcpy(paddedKey, hashed.data(), hashed.size());
	} else if (keyLength > 0) {
		std::memcpy(paddedKey, key, keyLength);
	}

	uint8_t innerPad[kSha256BlockSize];
	uint8_t outerPad[kSha256BlockSize];
	for (size_t i = 0; i < kSha256BlockSize; ++i) {
		innerPad[i] = static_cast<uint8_t>(paddedKey[i] ^ 0x36);
		outerPad[i] = static_cast<uint8_t>(paddedKey[i] ^ 0x5c);
	}

	std::vector<uint8_t> inner;
	inner.reserve(kSha256BlockSize + messageLength);
	inner.insert(inner.end(), innerPad, innerPad + kSha256BlockSize);
	if (messageLength > 0)
		inner.insert(inner.end(), message, message + messageLength);
	const Sha256Digest innerDigest = sha256(inner.data(), inner.size());

	uint8_t outer[kSha256BlockSize + kSha256DigestSize];
	std::memcpy(outer, outerPad, kSha256BlockSize);
	std::memcpy(outer + kSha256BlockSize, innerDigest.data(), innerDigest.size());

	return sha256(outer, sizeof(outer));
}

std::string toHex(const Sha256Digest &digest)
{
	static constexpr char kDigits[] = "0123456789abcdef";

	std::string hex;
	hex.reserve(digest.size() * 2);
	for (const uint8_t byte : digest) {
		hex.push_back(kDigits[byte >> 4]);
		hex.push_back(kDigits[byte & 0x0f]);
	}
	return hex;
}

std::string hmacSha256Hex(const std::string &key, const std::string &message)
{
	const Sha256Digest digest = hmacSha256(reinterpret_cast<const uint8_t *>(key.data()), key.size(),
					       reinterpret_cast<const uint8_t *>(message.data()), message.size());
	return toHex(digest);
}

} // namespace tmd
