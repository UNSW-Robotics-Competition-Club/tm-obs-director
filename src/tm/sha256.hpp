/*
obs-tm-director — VEX Tournament Manager automation for OBS Studio
Copyright (C) 2026 Sydney Robotics Academy <kevin.li@sydneyrobotics.com.au>

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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tmd {

constexpr size_t kSha256DigestSize = 32;
constexpr size_t kSha256BlockSize = 64;

using Sha256Digest = std::array<uint8_t, kSha256DigestSize>;

/// SHA-256 of `data`, per FIPS 180-4.
///
/// Written out rather than linked in. TM's request signing needs exactly one
/// primitive, and the alternative was mbedTLS - which obs-deps ships on Windows
/// and on no other platform, so depending on it meant the plugin could not build
/// on macOS or Linux at all. Two hundred lines of a fully specified hash, pinned
/// by the published test vectors, is a smaller price than that.
Sha256Digest sha256(const uint8_t *data, size_t length);

/// HMAC-SHA256 of `message` under `key`, per RFC 2104.
Sha256Digest hmacSha256(const uint8_t *key, size_t keyLength, const uint8_t *message, size_t messageLength);

/// HMAC-SHA256 as lower-case hex, which is the form TM's x-tm-signature takes.
std::string hmacSha256Hex(const std::string &key, const std::string &message);

/// Lower-case hex of a digest.
std::string toHex(const Sha256Digest &digest);

} // namespace tmd
