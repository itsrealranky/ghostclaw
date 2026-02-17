#include "ghostclaw/security/secrets.hpp"

#include "ghostclaw/config/config.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace ghostclaw::security {

namespace {

constexpr std::size_t NONCE_SIZE = 12;
constexpr std::size_t TAG_SIZE = 16;

bool is_prefixed_encrypted(const std::string &value) {
  return value.rfind("enc:", 0) == 0;
}

std::string b64_encode(const std::vector<unsigned char> &bytes) {
  const int output_len = 4 * static_cast<int>((bytes.size() + 2) / 3);
  std::string output(static_cast<std::size_t>(output_len), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char *>(output.data()), bytes.data(),
                  static_cast<int>(bytes.size()));
  return output;
}

common::Result<std::vector<unsigned char>> b64_decode(const std::string &text) {
  if (text.empty()) {
    return common::Result<std::vector<unsigned char>>::success({});
  }

  std::vector<unsigned char> decoded(text.size());
  const int len = EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char *>(text.data()),
                                  static_cast<int>(text.size()));
  if (len < 0) {
    return common::Result<std::vector<unsigned char>>::failure("Invalid base64 input");
  }

  std::size_t padding = 0;
  if (!text.empty() && text.back() == '=') {
    ++padding;
  }
  if (text.size() > 1 && text[text.size() - 2] == '=') {
    ++padding;
  }

  decoded.resize(static_cast<std::size_t>(len) - padding);
  return common::Result<std::vector<unsigned char>>::success(std::move(decoded));
}

common::Result<std::vector<unsigned char>>
chacha_encrypt(const SecretKey &key, const std::array<unsigned char, NONCE_SIZE> &nonce,
               const std::string &plaintext) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) {
    return common::Result<std::vector<unsigned char>>::failure("Failed to create cipher context");
  }

  std::vector<unsigned char> ciphertext(plaintext.size() + TAG_SIZE);
  int out_len = 0;
  int total_len = 0;

  auto cleanup = [&ctx]() { EVP_CIPHER_CTX_free(ctx); };

  if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1) {
    cleanup();
    return common::Result<std::vector<unsigned char>>::failure("Encrypt init failed");
  }
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, NONCE_SIZE, nullptr) != 1) {
    cleanup();
    return common::Result<std::vector<unsigned char>>::failure("Failed to set nonce size");
  }
  if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
    cleanup();
    return common::Result<std::vector<unsigned char>>::failure("Failed to set key/nonce");
  }
  if (EVP_EncryptUpdate(ctx, ciphertext.data(), &out_len,
                        reinterpret_cast<const unsigned char *>(plaintext.data()),
                        static_cast<int>(plaintext.size())) != 1) {
    cleanup();
    return common::Result<std::vector<unsigned char>>::failure("Encrypt update failed");
  }
  total_len += out_len;

  if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + total_len, &out_len) != 1) {
    cleanup();
    return common::Result<std::vector<unsigned char>>::failure("Encrypt final failed");
  }
  total_len += out_len;

  std::array<unsigned char, TAG_SIZE> tag{};
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, TAG_SIZE, tag.data()) != 1) {
    cleanup();
    return common::Result<std::vector<unsigned char>>::failure("Failed to get tag");
  }

  cleanup();
  ciphertext.resize(static_cast<std::size_t>(total_len));
  ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());
  return common::Result<std::vector<unsigned char>>::success(std::move(ciphertext));
}

common::Result<std::string>
chacha_decrypt(const SecretKey &key, const std::array<unsigned char, NONCE_SIZE> &nonce,
               const std::vector<unsigned char> &ciphertext_with_tag) {
  if (ciphertext_with_tag.size() < TAG_SIZE) {
    return common::Result<std::string>::failure("Ciphertext too short");
  }

  const std::size_t data_size = ciphertext_with_tag.size() - TAG_SIZE;
  const unsigned char *tag = ciphertext_with_tag.data() + data_size;

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) {
    return common::Result<std::string>::failure("Failed to create cipher context");
  }

  std::vector<unsigned char> plaintext(data_size + TAG_SIZE);
  int out_len = 0;
  int total_len = 0;

  auto cleanup = [&ctx]() { EVP_CIPHER_CTX_free(ctx); };

  if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1) {
    cleanup();
    return common::Result<std::string>::failure("Decrypt init failed");
  }
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, NONCE_SIZE, nullptr) != 1) {
    cleanup();
    return common::Result<std::string>::failure("Failed to set nonce size");
  }
  if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
    cleanup();
    return common::Result<std::string>::failure("Failed to set key/nonce");
  }

  if (EVP_DecryptUpdate(ctx, plaintext.data(), &out_len, ciphertext_with_tag.data(),
                        static_cast<int>(data_size)) != 1) {
    cleanup();
    return common::Result<std::string>::failure("Decrypt update failed");
  }
  total_len += out_len;

  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, TAG_SIZE,
                          const_cast<unsigned char *>(tag)) != 1) {
    cleanup();
    return common::Result<std::string>::failure("Failed to set tag");
  }

  if (EVP_DecryptFinal_ex(ctx, plaintext.data() + total_len, &out_len) != 1) {
    cleanup();
    return common::Result<std::string>::failure("Decryption failed");
  }
  total_len += out_len;

  cleanup();
  return common::Result<std::string>::success(
      std::string(reinterpret_cast<const char *>(plaintext.data()), static_cast<std::size_t>(total_len)));
}

void maybe_encrypt(std::optional<std::string> &value, const SecretKey &key) {
  if (!value.has_value() || value->empty() || is_prefixed_encrypted(*value)) {
    return;
  }
  const auto encrypted = encrypt_secret(key, *value);
  if (encrypted.ok()) {
    *value = "enc:" + encrypted.value();
  }
}

void maybe_decrypt(std::optional<std::string> &value, const SecretKey &key) {
  if (!value.has_value() || !is_prefixed_encrypted(*value)) {
    return;
  }
  const auto decrypted = decrypt_secret(key, value->substr(4));
  if (decrypted.ok()) {
    *value = decrypted.value();
  }
}

void maybe_encrypt_raw(std::string &value, const SecretKey &key) {
  if (value.empty() || is_prefixed_encrypted(value)) {
    return;
  }
  const auto encrypted = encrypt_secret(key, value);
  if (encrypted.ok()) {
    value = "enc:" + encrypted.value();
  }
}

void maybe_decrypt_raw(std::string &value, const SecretKey &key) {
  if (!is_prefixed_encrypted(value)) {
    return;
  }
  const auto decrypted = decrypt_secret(key, value.substr(4));
  if (decrypted.ok()) {
    value = decrypted.value();
  }
}

} // namespace

SecretKey generate_key() {
  SecretKey key{};
  RAND_bytes(key.data(), static_cast<int>(key.size()));
  return key;
}

common::Result<std::filesystem::path> key_path() {
  const auto cfg_dir = config::config_dir();
  if (!cfg_dir.ok()) {
    return common::Result<std::filesystem::path>::failure(cfg_dir.error());
  }
  return common::Result<std::filesystem::path>::success(cfg_dir.value() / "secrets.key");
}

common::Result<SecretKey> load_or_create_key() {
  const auto key_path_result = key_path();
  if (!key_path_result.ok()) {
    return common::Result<SecretKey>::failure(key_path_result.error());
  }

  const std::filesystem::path path = key_path_result.value();
  if (std::filesystem::exists(path)) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return common::Result<SecretKey>::failure("Failed to read key file");
    }
    SecretKey key{};
    in.read(reinterpret_cast<char *>(key.data()), static_cast<std::streamsize>(key.size()));
    if (in.gcount() != static_cast<std::streamsize>(key.size())) {
      return common::Result<SecretKey>::failure("Key file has invalid size");
    }
    return common::Result<SecretKey>::success(key);
  }

  const auto key = generate_key();
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return common::Result<SecretKey>::failure("Failed to write key file");
  }
  out.write(reinterpret_cast<const char *>(key.data()), static_cast<std::streamsize>(key.size()));
  out.close();

#ifndef _WIN32
  chmod(path.c_str(), 0600);
#endif

  return common::Result<SecretKey>::success(key);
}

common::Result<std::string> encrypt_secret(const SecretKey &key, const std::string &plaintext) {
  std::array<unsigned char, NONCE_SIZE> nonce{};
  RAND_bytes(nonce.data(), static_cast<int>(nonce.size()));

  const auto ciphertext = chacha_encrypt(key, nonce, plaintext);
  if (!ciphertext.ok()) {
    return common::Result<std::string>::failure(ciphertext.error());
  }

  std::vector<unsigned char> blob;
  blob.reserve(NONCE_SIZE + ciphertext.value().size());
  blob.insert(blob.end(), nonce.begin(), nonce.end());
  blob.insert(blob.end(), ciphertext.value().begin(), ciphertext.value().end());

  return common::Result<std::string>::success(b64_encode(blob));
}

common::Result<std::string> decrypt_secret(const SecretKey &key, const std::string &ciphertext) {
  const auto decoded = b64_decode(ciphertext);
  if (!decoded.ok()) {
    return common::Result<std::string>::failure(decoded.error());
  }

  if (decoded.value().size() < NONCE_SIZE + TAG_SIZE) {
    return common::Result<std::string>::failure("Ciphertext too short");
  }

  std::array<unsigned char, NONCE_SIZE> nonce{};
  std::copy_n(decoded.value().begin(), NONCE_SIZE, nonce.begin());

  std::vector<unsigned char> payload(decoded.value().begin() + static_cast<long>(NONCE_SIZE),
                                     decoded.value().end());

  return chacha_decrypt(key, nonce, payload);
}

void encrypt_config_secrets(config::Config &config, const SecretKey &key) {
  maybe_encrypt(config.api_key, key);
  maybe_encrypt(config.composio.api_key, key);

  if (config.channels.telegram.has_value()) {
    maybe_encrypt_raw(config.channels.telegram->bot_token, key);
  }
  if (config.channels.discord.has_value()) {
    maybe_encrypt_raw(config.channels.discord->bot_token, key);
  }
  if (config.channels.slack.has_value()) {
    maybe_encrypt_raw(config.channels.slack->bot_token, key);
  }
  if (config.channels.matrix.has_value()) {
    maybe_encrypt_raw(config.channels.matrix->access_token, key);
  }
  if (config.channels.whatsapp.has_value()) {
    maybe_encrypt_raw(config.channels.whatsapp->access_token, key);
  }
  if (config.channels.webhook.has_value()) {
    maybe_encrypt_raw(config.channels.webhook->secret, key);
  }
}

void decrypt_config_secrets(config::Config &config, const SecretKey &key) {
  maybe_decrypt(config.api_key, key);
  maybe_decrypt(config.composio.api_key, key);

  if (config.channels.telegram.has_value()) {
    maybe_decrypt_raw(config.channels.telegram->bot_token, key);
  }
  if (config.channels.discord.has_value()) {
    maybe_decrypt_raw(config.channels.discord->bot_token, key);
  }
  if (config.channels.slack.has_value()) {
    maybe_decrypt_raw(config.channels.slack->bot_token, key);
  }
  if (config.channels.matrix.has_value()) {
    maybe_decrypt_raw(config.channels.matrix->access_token, key);
  }
  if (config.channels.whatsapp.has_value()) {
    maybe_decrypt_raw(config.channels.whatsapp->access_token, key);
  }
  if (config.channels.webhook.has_value()) {
    maybe_decrypt_raw(config.channels.webhook->secret, key);
  }
}

} // namespace ghostclaw::security
