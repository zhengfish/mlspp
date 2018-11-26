#pragma once

#include "common.h"
#include "openssl/ec.h"
#include "openssl/evp.h"
#include "openssl/sha.h"
#include "tls_syntax.h"
#include <stdexcept>
#include <vector>

namespace mls {

// Interface cleanup wrapper for raw OpenSSL EVP keys
struct PKEYDeleter
{
  void operator()(EVP_PKEY* p) { EVP_PKEY_free(p); }
};

enum class OpenSSLKeyType : uint8_t
{
  P256,
  X25519
};

struct OpenSSLKey
{
public:
  OpenSSLKey() = default;

  OpenSSLKey(EVP_PKEY* key)
    : _key(key, PKEYDeleter{})
  {}

  virtual ~OpenSSLKey() = default;

  virtual size_t secret_size() const = 0;
  virtual size_t sig_size() const = 0;
  virtual bool can_derive() const = 0;

  virtual bytes marshal() const = 0;
  virtual void generate() = 0;
  virtual void set_public(const bytes& data) = 0;
  virtual void set_secret(const bytes& data) = 0;
  virtual OpenSSLKey* dup() const = 0;
  virtual OpenSSLKey* dup_public() const = 0;

  static OpenSSLKey* create(OpenSSLKeyType suite);

  bool operator==(const OpenSSLKey& other);

  bytes derive(const OpenSSLKey& pub);
  bytes sign(const bytes& message);
  bool verify(const bytes& message, const bytes& signature);

  std::unique_ptr<EVP_PKEY, PKEYDeleter> _key;
};

// Wrapper for OpenSSL errors
class OpenSSLError : public std::runtime_error
{
public:
  typedef std::runtime_error parent;
  using parent::parent;

  static OpenSSLError current();
};

// Scoped pointers for OpenSSL types
template<typename T>
void
TypedDelete(T* ptr);

template<typename T>
T*
TypedDup(T* ptr);

template<typename T>
class Scoped
{
public:
  Scoped()
    : _raw(nullptr)
  {}

  Scoped(T* raw) { adopt(raw); }

  Scoped(const Scoped& other) { adopt(TypedDup(other._raw)); }

  Scoped(Scoped&& other)
  {
    _raw = other._raw;
    other._raw = nullptr;
  }

  Scoped& operator=(const Scoped& other)
  {
    clear();
    adopt(TypedDup(other._raw));
    return *this;
  }

  Scoped& operator=(Scoped&& other)
  {
    _raw = other._raw;
    other._raw = nullptr;
    return *this;
  }

  ~Scoped() { clear(); }

  void move(Scoped& other)
  {
    adopt(other._raw);
    other._raw = nullptr;
  }

  void adopt(T* raw)
  {
    if (raw == nullptr) {
      throw OpenSSLError::current();
    }
    _raw = raw;
  }

  T* release()
  {
    T* out = _raw;
    _raw = nullptr;
    return out;
  }

  void clear()
  {
    if (_raw != nullptr) {
      TypedDelete(_raw);
      _raw = nullptr;
    }
  }

  const T* get() const { return _raw; }

  T* get() { return _raw; }

private:
  T* _raw;
};

class SHA256Digest
{
public:
  SHA256Digest();
  SHA256Digest(uint8_t byte);
  SHA256Digest(const bytes& data);

  SHA256Digest& write(uint8_t byte);
  SHA256Digest& write(const bytes& data);
  bytes digest();

  static const size_t output_size = 32;

private:
  SHA256_CTX _ctx;
};

bytes
zero_bytes(size_t size);

bytes
random_bytes(size_t size);

bytes
hkdf_extract(const bytes& salt, const bytes& ikm);

class State;

bytes
derive_secret(const bytes& secret,
              const std::string& label,
              const State& state,
              const size_t length);

class AESGCM
{
public:
  AESGCM() = delete;
  AESGCM(const AESGCM& other) = delete;
  AESGCM(AESGCM&& other) = delete;
  AESGCM& operator=(const AESGCM& other) = delete;
  AESGCM& operator=(AESGCM&& other) = delete;

  AESGCM(const bytes& key, const bytes& nonce);

  void set_aad(const bytes& key);
  bytes encrypt(const bytes& plaintext) const;
  bytes decrypt(const bytes& ciphertext) const;

  static const size_t key_size_128 = 16;
  static const size_t key_size_192 = 24;
  static const size_t key_size_256 = 32;
  static const size_t nonce_size = 12;
  static const size_t tag_size = 16;

private:
  bytes _key;
  bytes _nonce;
  bytes _aad;

  // This raw pointer only ever references memory managed by
  // OpenSSL, so it doesn't need to be scoped.
  const EVP_CIPHER* _cipher;
};

struct ECIESCiphertext;

class DHPublicKey
{
public:
  DHPublicKey();
  DHPublicKey(const DHPublicKey& other);
  DHPublicKey(DHPublicKey&& other);
  DHPublicKey(const bytes& data);
  DHPublicKey& operator=(const DHPublicKey& other);
  DHPublicKey& operator=(DHPublicKey&& other);

  bool operator==(const DHPublicKey& other) const;
  bool operator!=(const DHPublicKey& other) const;

  bytes to_bytes() const;
  void reset(const bytes& data);

  ECIESCiphertext encrypt(const bytes& plaintext) const;

private:
  std::unique_ptr<OpenSSLKey> _key;

  friend class DHPrivateKey;
};

tls::ostream&
operator<<(tls::ostream& out, const DHPublicKey& obj);
tls::istream&
operator>>(tls::istream& in, DHPublicKey& obj);

class DHPrivateKey
{
public:
  static DHPrivateKey generate();
  static DHPrivateKey derive(const bytes& secret);

  DHPrivateKey() = default;
  DHPrivateKey(const DHPrivateKey& other);
  DHPrivateKey(DHPrivateKey&& other);
  DHPrivateKey& operator=(const DHPrivateKey& other);
  DHPrivateKey& operator=(DHPrivateKey&& other);

  bool operator==(const DHPrivateKey& other) const;
  bool operator!=(const DHPrivateKey& other) const;

  bytes derive(const DHPublicKey& pub) const;
  const DHPublicKey& public_key() const;

  bytes decrypt(const ECIESCiphertext& ciphertext) const;

private:
  std::unique_ptr<OpenSSLKey> _key;
  DHPublicKey _pub;
};

struct ECIESCiphertext
{
  DHPublicKey ephemeral;
  tls::opaque<3> content;

  friend tls::ostream& operator<<(tls::ostream& out,
                                  const ECIESCiphertext& obj);
  friend tls::istream& operator>>(tls::istream& in, ECIESCiphertext& obj);
};

tls::ostream&
operator<<(tls::ostream& out, const DHPrivateKey& obj);
tls::istream&
operator>>(tls::istream& in, DHPrivateKey& obj);

// XXX(rlb@ipv.sx): There is a *ton* of repeated code between DH and
// Signature keys, both here and in the corresponding .cpp file.
// While this is unfortunate, it's a temporary state of affairs.  In
// the slightly longer run, we're going to want to refactor this to
// add more crypto agility anyway.  That agility will probably
// require a complete restructure of these classes, e.g., because
// Ed25519 does not use EC_KEY / ECDSA_sign.

class SignaturePublicKey
{
public:
  SignaturePublicKey();
  SignaturePublicKey(const SignaturePublicKey& other);
  SignaturePublicKey(SignaturePublicKey&& other);
  SignaturePublicKey(const bytes& data);
  SignaturePublicKey& operator=(const SignaturePublicKey& other);
  SignaturePublicKey& operator=(SignaturePublicKey&& other);

  bool operator==(const SignaturePublicKey& other) const;
  bool operator!=(const SignaturePublicKey& other) const;

  bool verify(const bytes& message, const bytes& signature) const;

  bytes to_bytes() const;
  void reset(const bytes& data);

private:
  std::unique_ptr<OpenSSLKey> _key;

  friend class SignaturePrivateKey;
};

tls::ostream&
operator<<(tls::ostream& out, const SignaturePublicKey& obj);
tls::istream&
operator>>(tls::istream& in, SignaturePublicKey& obj);

class SignaturePrivateKey
{
public:
  static SignaturePrivateKey generate();

  SignaturePrivateKey() = default;
  SignaturePrivateKey(const SignaturePrivateKey& other);
  SignaturePrivateKey(SignaturePrivateKey&& other);
  SignaturePrivateKey& operator=(const SignaturePrivateKey& other);
  SignaturePrivateKey& operator=(SignaturePrivateKey&& other);

  bool operator==(const SignaturePrivateKey& other) const;
  bool operator!=(const SignaturePrivateKey& other) const;

  bytes sign(const bytes& message) const;
  const SignaturePublicKey& public_key() const;

private:
  std::unique_ptr<OpenSSLKey> _key;
  SignaturePublicKey _pub;
};

} // namespace mls
