/*
 * Copyright 2019 The Project Oak Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "oak/server/storage/storage_processor.h"

#include <openssl/sha.h>

#include <algorithm>

#include "absl/memory/memory.h"
#include "absl/strings/escaping.h"
#include "asylo/crypto/aes_gcm_siv.h"
#include "asylo/crypto/nonce_generator.h"
#include "asylo/util/cleansing_types.h"
#include "grpcpp/create_channel.h"
#include "oak/common/logging.h"
#include "third_party/asylo/status_macros.h"

namespace oak {

namespace {

constexpr size_t kMaxMessageSize = 1 << 16;

absl::Status FromGrpcStatus(grpc::Status grpc_status) {
  return absl::Status(static_cast<absl::StatusCode>(grpc_status.error_code()),
                      grpc_status.error_message());
}

// If a gRPC client method returns a grpc::Status error, convert it to an
// absl::Status and return it.
#define RETURN_IF_GRPC_ERROR(expr)                          \
  do {                                                      \
    auto _grpc_status_to_verify = (expr);                   \
    if (ABSL_PREDICT_FALSE(!_grpc_status_to_verify.ok())) { \
      return FromGrpcStatus(_grpc_status_to_verify);        \
    }                                                       \
  } while (false)

// If a gRPC client method returns an asylo::Status error, convert it to an
// absl::Status and return it.
#define RETURN_IF_ASYLO_ERROR(expr)                                                               \
  do {                                                                                            \
    auto _asylo_status_to_verify = (expr);                                                        \
    if (ABSL_PREDICT_FALSE(!_asylo_status_to_verify.ok())) {                                      \
      return absl::Status(static_cast<absl::StatusCode>(_asylo_status_to_verify.CanonicalCode()), \
                          _asylo_status_to_verify.error_message());                               \
    }                                                                                             \
  } while (false)

std::string GetStorageId(const std::string& storage_name) {
  // TODO: Generate name-based UUID.
  return storage_name;
}

asylo::CleansingVector<uint8_t> GetStorageEncryptionKey(const std::string& /*storage_name*/) {
  // TODO: Request encryption key from escrow service.
  std::string encryption_key =
      absl::HexStringToBytes("c0dedeadc0dedeadc0dedeadc0dedeadc0dedeadc0dedeadc0dedeadc0dedead");
  return asylo::CleansingVector<uint8_t>(encryption_key.begin(), encryption_key.end());
}

constexpr size_t kAesGcmSivNonceSize = 12;
using NonceGenerator = asylo::NonceGenerator<kAesGcmSivNonceSize>;

// Produces fixed nonces using the storage encryption key and item name to
// allow deterministic encryption of the item name.
class FixedNonceGenerator : public NonceGenerator {
 public:
  FixedNonceGenerator(const std::string& item_name) : item_name_(item_name) {}

  // Called by asylo::AesGcmSiv::Seal.
  asylo::Status NextNonce(const std::vector<uint8_t>& key_id,
                          asylo::UnsafeBytes<kAesGcmSivNonceSize>* nonce) override {
    if (nonce == nullptr) {
      return asylo::Status(asylo::error::GoogleError::INTERNAL, "No output field provided");
    }

    // Generates a digest of the inputs to extract a fixed-size nonce.
    SHA256_CTX context;
    SHA256_Init(&context);
    SHA256_Update(&context, item_name_.data(), item_name_.size());
    SHA256_Update(&context, key_id.data(), key_id.size());
    std::vector<uint8_t> digest(SHA256_DIGEST_LENGTH);
    SHA256_Final(digest.data(), &context);
    std::copy_n(digest.begin(), kAesGcmSivNonceSize, nonce->begin());

    return asylo::Status::OkStatus();
  }

  // Causes asylo::AesGcmSiv::Seal to encrypt the nonce before use.
  bool uses_key_id() override { return true; }

 private:
  const std::string item_name_;
};

}  // namespace

StorageProcessor::StorageProcessor(const std::string& storage_address)
    : storage_service_(oak::Storage::NewStub(
          grpc::CreateChannel(storage_address, grpc::InsecureChannelCredentials()))) {}

const oak::StatusOr<std::string> StorageProcessor::EncryptItem(const std::string& item,
                                                               ItemType item_type) {
  // TODO: Replace "foo" with identifier for the encryption key.
  asylo::CleansingVector<uint8_t> key = GetStorageEncryptionKey("foo");
  asylo::CleansingString additional_authenticated_data;
  asylo::CleansingString nonce;
  asylo::CleansingString item_encrypted;

  NonceGenerator* nonce_generator;
  switch (item_type) {
    case ItemType::NAME:
      nonce_generator = new FixedNonceGenerator(item);
      break;
    case ItemType::VALUE:
      nonce_generator = new asylo::AesGcmSivNonceGenerator();
      break;
  }
  // Create a cryptor, passing ownership of the nonce generator to it.
  asylo::AesGcmSivCryptor cryptor(kMaxMessageSize, nonce_generator);
  RETURN_IF_ASYLO_ERROR(
      cryptor.Seal(key, additional_authenticated_data, item, &nonce, &item_encrypted));

  return absl::StrCat(nonce, item_encrypted);
}

const oak::StatusOr<std::string> StorageProcessor::DecryptItem(const std::string& input, ItemType) {
  asylo::CleansingString input_clean(input.data(), input.size());

  if (input_clean.size() < kAesGcmSivNonceSize) {
    return absl::Status(absl::StatusCode::kInvalidArgument,
                        absl::StrCat("Input too short: expected at least ", kAesGcmSivNonceSize,
                                     " bytes, got ", input_clean.size()));
  }

  // TODO: Replace "foo" with identifier for the encryption key.
  asylo::CleansingVector<uint8_t> key(GetStorageEncryptionKey("foo"));
  asylo::CleansingString additional_authenticated_data;
  asylo::CleansingString nonce = input_clean.substr(0, kAesGcmSivNonceSize);
  asylo::CleansingString item_encrypted = input_clean.substr(kAesGcmSivNonceSize);
  asylo::CleansingString item_decrypted;

  // The nonce generator is not used for decryption.
  asylo::AesGcmSivCryptor cryptor(kMaxMessageSize, nullptr);
  RETURN_IF_ASYLO_ERROR(
      cryptor.Open(key, additional_authenticated_data, item_encrypted, nonce, &item_decrypted));

  return std::string(item_decrypted.data(), item_decrypted.size());
}

oak::StatusOr<std::string> StorageProcessor::Read(const std::string& storage_name,
                                                  const std::string& item_name,
                                                  const std::string& transaction_id) {
  StorageReadRequest read_request;
  read_request.set_storage_id(GetStorageId(storage_name));
  if (!transaction_id.empty()) {
    read_request.set_transaction_id(transaction_id);
  }
  std::string name;
  ASYLO_ASSIGN_OR_RETURN(name, EncryptItem(item_name, ItemType::NAME));
  read_request.set_item_name(name);

  grpc::ClientContext context;
  StorageReadResponse read_response;
  RETURN_IF_GRPC_ERROR(storage_service_->Read(&context, read_request, &read_response));
  return DecryptItem(read_response.item_value(), ItemType::VALUE);
}

absl::Status StorageProcessor::Write(const std::string& storage_name, const std::string& item_name,
                                     const std::string& item_value,
                                     const std::string& transaction_id) {
  StorageWriteRequest write_request;
  write_request.set_storage_id(GetStorageId(storage_name));
  if (!transaction_id.empty()) {
    write_request.set_transaction_id(transaction_id);
  }
  std::string name;
  ASYLO_ASSIGN_OR_RETURN(name, EncryptItem(item_name, ItemType::NAME));
  write_request.set_item_name(name);

  std::string value;
  ASYLO_ASSIGN_OR_RETURN(value, EncryptItem(item_value, ItemType::VALUE));
  write_request.set_item_value(value);

  grpc::ClientContext context;
  StorageWriteResponse write_response;
  return FromGrpcStatus(storage_service_->Write(&context, write_request, &write_response));
}

absl::Status StorageProcessor::Delete(const std::string& storage_name, const std::string& item_name,
                                      const std::string& transaction_id) {
  StorageDeleteRequest delete_request;
  delete_request.set_storage_id(GetStorageId(storage_name));
  if (!transaction_id.empty()) {
    delete_request.set_transaction_id(transaction_id);
  }
  std::string name;
  ASYLO_ASSIGN_OR_RETURN(name, EncryptItem(item_name, ItemType::NAME));
  delete_request.set_item_name(name);

  grpc::ClientContext context;
  StorageDeleteResponse delete_response;
  return FromGrpcStatus(storage_service_->Delete(&context, delete_request, &delete_response));
}

oak::StatusOr<std::string> StorageProcessor::Begin(const std::string& storage_name) {
  StorageBeginRequest begin_request;
  begin_request.set_storage_id(GetStorageId(storage_name));

  grpc::ClientContext context;
  StorageBeginResponse begin_response;
  RETURN_IF_GRPC_ERROR(storage_service_->Begin(&context, begin_request, &begin_response));
  return oak::StatusOr<std::string>(begin_response.transaction_id());
}

absl::Status StorageProcessor::Commit(const std::string& storage_name,
                                      const std::string& transaction_id) {
  StorageCommitRequest commit_request;
  commit_request.set_storage_id(GetStorageId(storage_name));
  commit_request.set_transaction_id(transaction_id);

  grpc::ClientContext context;
  StorageCommitResponse commit_response;
  return FromGrpcStatus(storage_service_->Commit(&context, commit_request, &commit_response));
}

absl::Status StorageProcessor::Rollback(const std::string& storage_name,
                                        const std::string& transaction_id) {
  StorageRollbackRequest rollback_request;
  rollback_request.set_storage_id(GetStorageId(storage_name));
  rollback_request.set_transaction_id(transaction_id);

  grpc::ClientContext context;
  StorageRollbackResponse rollback_response;
  return FromGrpcStatus(storage_service_->Rollback(&context, rollback_request, &rollback_response));
}

}  // namespace oak
