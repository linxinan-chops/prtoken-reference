// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <arpa/inet.h>
#include <netinet/in.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "ortools/base/path.h"
#include "prtoken/issuer.h"
#include "prtoken/storage.h"
#include "prtoken/token.h"
#include "prtoken/token.pb.h"

ABSL_FLAG(int, num_tokens, 100, "How many tokens to generate.");
ABSL_FLAG(float, p_reveal, 0.1, "p_reveal value.");
ABSL_FLAG(std::string, ip, "", "IPv4 or v6 address in string");
ABSL_FLAG(std::string, output_dir, "/tmp/", "Where to store output data.");
ABSL_FLAG(std::string, custom_db_filename, "",
          "Append to this file in the output_dir instead of a per-epoch DB.");

namespace {

constexpr int kErrorKeyGeneration = 1;
constexpr int kErrorSignalParsing = 2;
constexpr int kErrorTokenIssuance = 3;
constexpr int kErrorKeyWrite = 4;
constexpr int kErrorTokenWrite = 5;

// This is an alias to aid readability.
constexpr size_t SignalSizeLimit = prtoken::token_structure.signal_size;

// Helper to check if the input is a valid IP address.
bool IsValidIPAddress(absl::string_view ip_string) {
  struct in_addr ip4_addr;
  struct in6_addr ip6_addr;
  return (inet_pton(AF_INET, ip_string.data(), &ip4_addr) == 1 ||
          inet_pton(AF_INET6, ip_string.data(), &ip6_addr) == 1);
}

// Transform IP string into byte array. V4 address is padded
// to IPv4-mapped address, see
// http://tools.ietf.org/html/rfc3493#section-3.7.
absl::StatusOr<std::array<uint8_t, SignalSizeLimit>> IPStringToByteArray(
    std::string_view ip_string) {
  std::array<uint8_t, SignalSizeLimit> ipv6_bytes;
  struct in6_addr ip6_addr;
  if (inet_pton(AF_INET6, ip_string.data(), &ip6_addr) == 1) {
    std::memcpy(ipv6_bytes.data(), &ip6_addr, SignalSizeLimit);
    return ipv6_bytes;
  }
  struct in_addr ipv4_addr;
  if (inet_pton(AF_INET, ip_string.data(), &ipv4_addr) == 1) {
    // IPv4-mapped IPv6 format: ::ffff:IPv4
    // First 10 bytes are 0, next 2 bytes are 0xff, then the IPv4 address.
    std::memset(ipv6_bytes.data(), 0, 10);
    ipv6_bytes[10] = 0xff;
    ipv6_bytes[11] = 0xff;
    std::memcpy(ipv6_bytes.data() + 12, &ipv4_addr, 4);
    return ipv6_bytes;
  }
  return absl::InvalidArgumentError("Invalid IPv4 or IPv6 address.");
}

// Function to generate and store tokens
absl::Status GenerateAndStoreTokens() {
  std::string ip = absl::GetFlag(FLAGS_ip);
  if (!IsValidIPAddress(ip)) {
    return absl::InvalidArgumentError("Invalid IP address.");
  }
  int num_tokens = absl::GetFlag(FLAGS_num_tokens);
  float p_reveal = absl::GetFlag(FLAGS_p_reveal);
  std::string output_dir = absl::GetFlag(FLAGS_output_dir);
  std::string custom_db_filename = absl::GetFlag(FLAGS_custom_db_filename);

  // Generate secrets and instantiate an issuer.
  absl::StatusOr<prtoken::proto::ElGamalKeyMaterial> keypair_or =
      prtoken::GenerateElGamalKeypair();
  if (!keypair_or.ok()) {
    return absl::InternalError("Failed to generate ElGamal keypair.");
  }
  const prtoken::proto::ElGamalKeyMaterial &keypair = *keypair_or;
  const std::string secret_key_hmac = prtoken::GenerateSecretKeyHMAC();
  absl::StatusOr<std::unique_ptr<prtoken::Issuer>> issuer_status =
      prtoken::Issuer::Create(secret_key_hmac, keypair.public_key());
  if (!issuer_status.ok()) {
    return absl::InternalError("Failed to create issuer.");
  }
  std::unique_ptr<prtoken::Issuer> issuer(std::move(issuer_status.value()));

  // Mint tokens.
  std::vector<private_join_and_compute::ElGamalCiphertext> tokens;
  absl::StatusOr<std::array<uint8_t, SignalSizeLimit>> signal_or =
      IPStringToByteArray(ip);
  if (!signal_or.ok()) {
    return signal_or.status();
  }
  std::array<uint8_t, SignalSizeLimit> signal = *signal_or;
  absl::Status status = issuer->IssueTokens(
      signal, static_cast<int>(p_reveal * num_tokens), num_tokens, tokens);
  if (!status.ok()) {
    return absl::InternalError("Failed to issue tokens.");
  }

  const absl::Time epoch_start_time = absl::Now();
  const absl::Time epoch_end_time = absl::Now() + absl::Hours(24);
  // Format epoch_end_time as a string in ISO 8601 UST format YYYYMMDDHHMMSS.
  std::string epoch_end_time_str =
      absl::FormatTime("%Y%m%d%H%M%S", epoch_end_time, absl::UTCTimeZone());
  const std::string key_file = file::JoinPath(
      output_dir, absl::StrCat("keys-", epoch_end_time_str, ".json"));
  std::string tokens_db_file;
  if (!custom_db_filename.empty()) {
    tokens_db_file.assign(file::JoinPath(output_dir, custom_db_filename));
  } else {
    tokens_db_file =
        absl::StrCat(output_dir, "/tokens-", epoch_end_time_str, ".db");
  }
  // Write keys and tokens.
  if (!prtoken::WriteKeysToFile(keypair, secret_key_hmac, key_file,
                                epoch_start_time, epoch_end_time)
           .ok()) {
    return absl::InternalError("Failed to write keys to file.");
  }
  if (!prtoken::WriteTokensToFile(tokens, keypair.public_key(), p_reveal,
                                  epoch_end_time, tokens_db_file)
           .ok()) {
    return absl::InternalError("Failed to write tokens to file.");
  }
  return absl::OkStatus();
}

}  // namespace

int main(int argc, char **argv) {
  absl::InitializeLog();

  // For Positional Arguments.
  std::vector<char *> pop_args = absl::ParseCommandLine(argc, argv);
  if (pop_args.size() < 2) {
    LOG(ERROR) << "Usage: " << argv[0] << " <issue|verify> [options]\n";
    return 1;
  }

  std::string command = pop_args[1];
  if (command == "issue") {
    absl::Status status = GenerateAndStoreTokens();
    if (status.ok()) return 0;
    LOG(ERROR) << status.message();
    return 1;
  } else if (command == "verify") {
    // TODO(b/400517728): Add code for token decryption.
    return 0;
  }
  LOG(ERROR) << "Usage: prtoken <issue|verify> [options]\n"
             << "but got: " << command << "\n";
  return 1;
}
