#include "crypto.h"
#include "common.h"
#include "openssl/ecdh.h"
#include "openssl/ecdsa.h"
#include "openssl/err.h"
#include "openssl/evp.h"
#include "openssl/hmac.h"
#include "openssl/obj_mac.h"
#include "openssl/rand.h"
#include "openssl/sha.h"
#include "state.h"

#include <iostream>
#include <string>

#define DH_KEY_TYPE OpenSSLKeyType::P256
// #define DH_KEY_TYPE OpenSSLKeyType::X25519

#define SIG_KEY_TYPE OpenSSLKeyType::P256

#define DH_CURVE NID_X9_62_prime256v1
#define SIG_CURVE NID_X9_62_prime256v1
#define DH_OUTPUT_BYTES SHA256_DIGEST_LENGTH

namespace mls {

// Things we need to do per-key-type:
// * TypedDup
// * to_bytes
// * from_bytes
// * construct from data

///
/// OpenSSLKey
///
/// This is used to encapsulate the operations required for
/// different types of points, with a slightly cleaner interface
/// than OpenSSL's EVP interface.
///

struct X25519Key : OpenSSLKey
{
public:
  X25519Key() = default;

  X25519Key(EVP_PKEY* pkey)
    : OpenSSLKey(pkey)
  {}

  virtual size_t secret_size() const { return 32; }
  virtual size_t sig_size() const { return 200; }
  virtual bool can_derive() const { return true; }

  virtual bytes marshal() const
  {
    size_t raw_len;
    if (1 != EVP_PKEY_get_raw_public_key(_key.get(), nullptr, &raw_len)) {
      throw OpenSSLError::current();
    }

    bytes raw(raw_len);
    uint8_t* data_ptr = raw.data();
    if (1 != EVP_PKEY_get_raw_public_key(_key.get(), data_ptr, &raw_len)) {
      throw OpenSSLError::current();
    }

    return raw;
  }

  virtual void generate() { set_secret(random_bytes(secret_size())); }

  virtual void set_public(const bytes& data)
  {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
      EVP_PKEY_X25519, nullptr, data.data(), data.size());
    if (!pkey) {
      throw OpenSSLError::current();
    }

    _key.reset(pkey);
  }

  virtual void set_secret(const bytes& data)
  {
    bytes digest = SHA256Digest(dh_hash_prefix).write(data).digest();

    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
      EVP_PKEY_X25519, nullptr, digest.data(), digest.size());
    if (!pkey) {
      throw OpenSSLError::current();
    }

    _key.reset(pkey);
  }

  virtual OpenSSLKey* dup() const
  {
    size_t raw_len = 0;
    if (1 != EVP_PKEY_get_raw_private_key(_key.get(), nullptr, &raw_len)) {
      throw OpenSSLError::current();
    }

    // The actual key fetch will fail if `_key` represents a public key
    bytes raw(raw_len);
    auto data_ptr = raw.data();
    auto rv = EVP_PKEY_get_raw_private_key(_key.get(), data_ptr, &raw_len);
    if (rv == 1) {
      auto pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_X25519, nullptr, raw.data(), raw.size());
      if (!pkey) {
        throw OpenSSLError::current();
      }

      return new X25519Key(pkey);
    }

    return dup_public();
  }

  virtual OpenSSLKey* dup_public() const
  {
    size_t raw_len = 0;
    if (1 != EVP_PKEY_get_raw_public_key(_key.get(), nullptr, &raw_len)) {
      throw OpenSSLError::current();
    }

    bytes raw(raw_len);
    auto data_ptr = raw.data();
    if (1 != EVP_PKEY_get_raw_public_key(_key.get(), data_ptr, &raw_len)) {
      throw OpenSSLError::current();
    }

    auto pkey = EVP_PKEY_new_raw_public_key(
      EVP_PKEY_X25519, nullptr, raw.data(), raw.size());
    if (!pkey) {
      throw OpenSSLError::current();
    }

    return new X25519Key(pkey);
  }
};

struct P256Key : OpenSSLKey
{
public:
  P256Key() = default;

  P256Key(EVP_PKEY* pkey)
    : OpenSSLKey(pkey)
  {}

  virtual size_t secret_size() const { return 32; }
  virtual size_t sig_size() const { return 200; }
  virtual bool can_derive() const { return true; }

  virtual bytes marshal() const
  {
    auto pub = EVP_PKEY_get0_EC_KEY(_key.get());

    auto len = i2o_ECPublicKey(pub, nullptr);
    if (len == 0) {
      // Technically, this is not necessarily an error, but in
      // practice it always will be.
      throw OpenSSLError::current();
    }

    bytes out(len);
    auto data = out.data();
    if (i2o_ECPublicKey(pub, &data) == 0) {
      throw OpenSSLError::current();
    }

    return out;
  }

  virtual void generate()
  {
    Scoped<EC_KEY> eckey = new_ec_key();
    if (1 != EC_KEY_generate_key(eckey.get())) {
      throw OpenSSLError::current();
    }

    reset(eckey.release());
  }

  virtual void set_public(const bytes& data)
  {
    auto eckey = Scoped<EC_KEY>(new_ec_key());

    auto eckey_ptr = eckey.get();
    auto data_ptr = data.data();
    if (!o2i_ECPublicKey(&eckey_ptr, &data_ptr, data.size())) {
      throw OpenSSLError::current();
    }

    reset(eckey.release());
  }

  virtual void set_secret(const bytes& data)
  {
    bytes digest = SHA256Digest(dh_hash_prefix).write(data).digest();

    EC_KEY* eckey = new_ec_key();

    auto group = EC_KEY_get0_group(eckey);
    Scoped<BIGNUM> d = BN_bin2bn(digest.data(), digest.size(), nullptr);
    Scoped<EC_POINT> pt = EC_POINT_new(group);
    EC_POINT_mul(group, pt.get(), d.get(), nullptr, nullptr, nullptr);

    EC_KEY_set_private_key(eckey, d.get());
    EC_KEY_set_public_key(eckey, pt.get());

    reset(eckey);
  }

  virtual OpenSSLKey* dup() const
  {
    auto eckey_out = EC_KEY_dup(my_ec_key());
    return new P256Key(eckey_out);
  }

  virtual OpenSSLKey* dup_public() const
  {
    auto eckey = my_ec_key();
    auto group = EC_KEY_get0_group(eckey);
    auto point = EC_KEY_get0_public_key(eckey);

    auto eckey_out = new_ec_key();
    EC_KEY_set_public_key(eckey_out, point);
    return new P256Key(eckey_out);
  }

private:
  static const int _curve_nid = NID_X9_62_prime256v1;

  P256Key(EC_KEY* eckey)
    : OpenSSLKey()
  {
    reset(eckey);
  }

  void reset(EC_KEY* eckey)
  {
    auto pkey = EVP_PKEY_new();
    EVP_PKEY_assign_EC_KEY(pkey, eckey);
    _key.reset(pkey);
  }

  const EC_KEY* my_ec_key() const { return EVP_PKEY_get0_EC_KEY(_key.get()); }

  EC_KEY* new_ec_key() const { return EC_KEY_new_by_curve_name(_curve_nid); }
};

bool
OpenSSLKey::operator==(const OpenSSLKey& other)
{
  // If one pointer is null and the other is not, then the two keys
  // are not equal
  if (!!_key.get() != !!other._key.get()) {
    return false;
  }

  // If both pointers are null, then the two keys are equal.
  if (!_key.get()) {
    return true;
  }

  auto cmp = EVP_PKEY_cmp(_key.get(), other._key.get());
  return cmp == 1;
}

OpenSSLKey*
OpenSSLKey::create(OpenSSLKeyType type)
{
  switch (type) {
    case OpenSSLKeyType::X25519:
      return new X25519Key;
    case OpenSSLKeyType::P256:
      return new P256Key;
  }
}

bytes
OpenSSLKey::derive(const OpenSSLKey& pub)
{
  if (!can_derive() || !pub.can_derive()) {
    throw InvalidParameterError("Inappropriate key(s) for derive");
  }

  EVP_PKEY* priv_pkey = const_cast<EVP_PKEY*>(_key.get());
  EVP_PKEY* pub_pkey = const_cast<EVP_PKEY*>(pub._key.get());

  Scoped<EVP_PKEY_CTX> ctx(EVP_PKEY_CTX_new(priv_pkey, nullptr));
  if (!ctx.get()) {
    throw OpenSSLError::current();
  }

  if (1 != EVP_PKEY_derive_init(ctx.get())) {
    throw OpenSSLError::current();
  }

  if (1 != EVP_PKEY_derive_set_peer(ctx.get(), pub_pkey)) {
    throw OpenSSLError::current();
  }

  size_t out_len;
  if (1 != EVP_PKEY_derive(ctx.get(), nullptr, &out_len)) {
    throw OpenSSLError::current();
  }

  bytes out(out_len);
  uint8_t* ptr = out.data();
  if (1 != (EVP_PKEY_derive(ctx.get(), ptr, &out_len))) {
    throw OpenSSLError::current();
  }

  return out;
}

bytes
OpenSSLKey::sign(const bytes& msg)
{
  Scoped<EVP_MD_CTX> ctx = EVP_MD_CTX_create();
  if (!ctx.get()) {
    throw OpenSSLError::current();
  }

  if (1 != EVP_DigestSignInit(ctx.get(), NULL, NULL, NULL, _key.get())) {
    throw OpenSSLError::current();
  }

  size_t siglen = sig_size();
  bytes sig(sig_size());
  if (1 !=
      EVP_DigestSign(ctx.get(), sig.data(), &siglen, msg.data(), msg.size())) {
    throw OpenSSLError::current();
  }

  sig.resize(siglen);
  return sig;
}

bool
OpenSSLKey::verify(const bytes& msg, const bytes& sig)
{
  Scoped<EVP_MD_CTX> ctx = EVP_MD_CTX_create();
  if (!ctx.get()) {
    throw OpenSSLError::current();
  }

  if (1 != EVP_DigestVerifyInit(ctx.get(), NULL, NULL, NULL, _key.get())) {
    throw OpenSSLError::current();
  }

  auto rv =
    EVP_DigestVerify(ctx.get(), sig.data(), sig.size(), msg.data(), msg.size());

  return rv == 1;
}

///
/// OpenSSLError
///

OpenSSLError
OpenSSLError::current()
{
  unsigned long code = ERR_get_error();
  return OpenSSLError(ERR_error_string(code, nullptr));
}

template<>
void
TypedDelete(EVP_PKEY* ptr)
{
  EVP_PKEY_free(ptr);
}

template<>
void
TypedDelete(EVP_PKEY_CTX* ptr)
{
  EVP_PKEY_CTX_free(ptr);
}

template<>
void
TypedDelete(EC_KEY* ptr)
{
  EC_KEY_free(ptr);
}

template<>
void
TypedDelete(EC_GROUP* ptr)
{
  EC_GROUP_free(ptr);
}

template<>
void
TypedDelete(EC_POINT* ptr)
{
  EC_POINT_free(ptr);
}

template<>
void
TypedDelete(BIGNUM* ptr)
{
  BN_free(ptr);
}

template<>
void
TypedDelete(EVP_CIPHER_CTX* ptr)
{
  EVP_CIPHER_CTX_free(ptr);
}

template<>
void
TypedDelete(EVP_MD_CTX* ptr)
{
  EVP_MD_CTX_free(ptr);
}

template<>
EC_KEY*
TypedDup(EC_KEY* ptr)
{
  return EC_KEY_dup(ptr);
}

template<>
EC_GROUP*
TypedDup(EC_GROUP* ptr)
{
  return EC_GROUP_dup(ptr);
}

template<>
EVP_PKEY*
TypedDup(EVP_PKEY* ptr)
{
  // TODO fork on key type

  size_t raw_len = 0;
  if (1 != EVP_PKEY_get_raw_private_key(ptr, nullptr, &raw_len)) {
    throw OpenSSLError::current();
  }

  // The actual key fetch will fail if `ptr` represents a public key
  bytes raw(raw_len);
  uint8_t* data_ptr = raw.data();
  int rv = EVP_PKEY_get_raw_private_key(ptr, data_ptr, &raw_len);
  if (rv == 1) {
    return EVP_PKEY_new_raw_private_key(
      EVP_PKEY_X25519, nullptr, raw.data(), raw.size());
  }

  if (1 != EVP_PKEY_get_raw_public_key(ptr, nullptr, &raw_len)) {
    throw OpenSSLError::current();
  }

  raw.resize(raw_len);
  data_ptr = raw.data();
  if (1 != EVP_PKEY_get_raw_public_key(ptr, data_ptr, &raw_len)) {
    throw OpenSSLError::current();
  }

  return EVP_PKEY_new_raw_public_key(
    EVP_PKEY_X25519, nullptr, raw.data(), raw.size());
}

Scoped<EC_GROUP>
defaultECGroup()
{
  Scoped<EC_GROUP> group = EC_GROUP_new_by_curve_name(DH_CURVE);
  if (group.get() == nullptr) {
    throw OpenSSLError::current();
  }
  return group;
}

///
/// SHA256Digest
///

SHA256Digest::SHA256Digest()
{
  if (SHA256_Init(&_ctx) != 1) {
    throw OpenSSLError::current();
  }
}

SHA256Digest::SHA256Digest(uint8_t byte)
  : SHA256Digest()
{
  write(byte);
}

SHA256Digest::SHA256Digest(const bytes& data)
  : SHA256Digest()
{
  write(data);
}

SHA256Digest&
SHA256Digest::write(uint8_t byte)
{
  if (SHA256_Update(&_ctx, &byte, 1) != 1) {
    throw OpenSSLError::current();
  }
  return *this;
}

SHA256Digest&
SHA256Digest::write(const bytes& data)
{
  if (SHA256_Update(&_ctx, data.data(), data.size()) != 1) {
    throw OpenSSLError::current();
  }
  return *this;
}

bytes
SHA256Digest::digest()
{
  bytes out(SHA256_DIGEST_LENGTH);
  if (SHA256_Final(out.data(), &_ctx) != 1) {
    throw OpenSSLError::current();
  }
  return out;
}

///
/// HKDF and DeriveSecret
///

static bytes
hmac_sha256(const bytes& key, const bytes& data)
{
  unsigned int size = 0;
  bytes md(SHA256_DIGEST_LENGTH);
  if (!HMAC(EVP_sha256(),
            key.data(),
            key.size(),
            data.data(),
            data.size(),
            md.data(),
            &size)) {
    throw OpenSSLError::current();
  }

  return md;
}

bytes
hkdf_extract(const bytes& salt, const bytes& ikm)
{
  return hmac_sha256(salt, ikm);
}

// struct {
//     uint16 length = Length;
//     opaque label<6..255> = "mls10 " + Label;
//     GroupState state = State;
// } HkdfLabel;
struct HKDFLabel
{
  uint16_t length;
  tls::opaque<1, 7> label;
  State group_state;
};

tls::ostream&
operator<<(tls::ostream& out, const HKDFLabel& obj)
{
  return out << obj.length << obj.label << obj.group_state;
}

bytes
zero_bytes(size_t size)
{
  bytes out(size);
  for (auto& b : out) {
    b = 0;
  }
  return out;
}

bytes
random_bytes(size_t size)
{
  bytes out(size);
  if (!RAND_bytes(out.data(), out.size())) {
    throw OpenSSLError::current();
  }
  return out;
}

// XXX: This method requires that size <= Hash.length, so that
// HKDF-Expand(Secret, Label) reduces to:
//
//   HMAC(Secret, Label || 0x01)
template<typename T>
static bytes
hkdf_expand(const bytes& secret, const T& info, size_t size)
{
  auto label = tls::marshal(info);
  label.push_back(0x01);
  auto mac = hmac_sha256(secret, label);
  mac.resize(size);
  return mac;
}

bytes
derive_secret(const bytes& secret,
              const std::string& label,
              const State& state,
              size_t size)
{
  std::string mls_label = std::string("mls10 ") + label;
  bytes vec_label(mls_label.begin(), mls_label.end());

  HKDFLabel label_str{ uint16_t(size), vec_label, state };
  return hkdf_expand(secret, label_str, size);
}

///
/// AESGCM
///

AESGCM::AESGCM(const bytes& key, const bytes& nonce)
{
  switch (key.size()) {
    case key_size_128:
      _cipher = EVP_aes_128_gcm();
      break;
    case key_size_192:
      _cipher = EVP_aes_192_gcm();
      break;
    case key_size_256:
      _cipher = EVP_aes_256_gcm();
      break;
    default:
      throw InvalidParameterError("Invalid AES key size");
  }

  if (nonce.size() != nonce_size) {
    throw InvalidParameterError("Invalid AES-GCM nonce size");
  }

  _key = key;
  _nonce = nonce;
}

void
AESGCM::set_aad(const bytes& aad)
{
  _aad = aad;
}

bytes
AESGCM::encrypt(const bytes& pt) const
{
  Scoped<EVP_CIPHER_CTX> ctx = EVP_CIPHER_CTX_new();
  if (ctx.get() == nullptr) {
    throw OpenSSLError::current();
  }

  if (!EVP_EncryptInit(ctx.get(), _cipher, _key.data(), _nonce.data())) {
    throw OpenSSLError::current();
  }

  int outlen = pt.size() + tag_size;
  bytes ct(pt.size() + tag_size);

  if (_aad.size() > 0) {
    if (!EVP_EncryptUpdate(
          ctx.get(), nullptr, &outlen, _aad.data(), _aad.size())) {
      throw OpenSSLError::current();
    }
  }

  if (!EVP_EncryptUpdate(ctx.get(), ct.data(), &outlen, pt.data(), pt.size())) {
    throw OpenSSLError::current();
  }

  if (!EVP_EncryptFinal(ctx.get(), ct.data() + pt.size(), &outlen)) {
    throw OpenSSLError::current();
  }

  if (!EVP_CIPHER_CTX_ctrl(
        ctx.get(), EVP_CTRL_GCM_GET_TAG, tag_size, ct.data() + pt.size())) {
    throw OpenSSLError::current();
  }

  return ct;
}

bytes
AESGCM::decrypt(const bytes& ct) const
{
  if (ct.size() < tag_size) {
    throw InvalidParameterError("AES-GCM ciphertext smaller than tag size");
  }

  Scoped<EVP_CIPHER_CTX> ctx = EVP_CIPHER_CTX_new();
  if (ctx.get() == nullptr) {
    throw OpenSSLError::current();
  }

  if (!EVP_DecryptInit(ctx.get(), _cipher, _key.data(), _nonce.data())) {
    throw OpenSSLError::current();
  }

  uint8_t* tag = const_cast<uint8_t*>(ct.data() + ct.size() - tag_size);
  if (!EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, tag_size, tag)) {
    throw OpenSSLError::current();
  }

  int dummy;
  if (_aad.size() > 0) {
    if (!EVP_DecryptUpdate(
          ctx.get(), nullptr, &dummy, _aad.data(), _aad.size())) {
      throw OpenSSLError::current();
    }
  }

  bytes pt(ct.size() - tag_size);
  if (!EVP_DecryptUpdate(
        ctx.get(), pt.data(), &dummy, ct.data(), ct.size() - tag_size)) {
    throw OpenSSLError::current();
  }

  if (!EVP_DecryptFinal(ctx.get(), pt.data() + ct.size() - tag_size, &dummy)) {
    throw OpenSSLError::current();
  }

  return pt;
}

///
/// DHPublicKey
///

DHPublicKey::DHPublicKey()
  : _key(OpenSSLKey::create(DH_KEY_TYPE))
{}

DHPublicKey::DHPublicKey(const DHPublicKey& other)
  : _key(other._key->dup())
{}

DHPublicKey::DHPublicKey(DHPublicKey&& other)
  : _key(std::move(other._key))
{}

DHPublicKey::DHPublicKey(const bytes& data)
  : _key(OpenSSLKey::create(DH_KEY_TYPE))
{
  reset(data);
}

DHPublicKey&
DHPublicKey::operator=(const DHPublicKey& other)
{
  if (&other != this) {
    _key.reset(other._key->dup());
  }
  return *this;
}

DHPublicKey&
DHPublicKey::operator=(DHPublicKey&& other)
{
  if (&other != this) {
    _key = std::move(other._key);
  }
  return *this;
}

bool
DHPublicKey::operator==(const DHPublicKey& other) const
{
  return *_key == *other._key;
}

bool
DHPublicKey::operator!=(const DHPublicKey& other) const
{
  return !(*this == other);
}

bytes
DHPublicKey::to_bytes() const
{
  return _key->marshal();
}

void
DHPublicKey::reset(const bytes& data)
{
  _key->set_public(data);
}

// key = HKDF-Expand(Secret, ECIESLabel("key"), Length)
// nonce = HKDF-Expand(Secret, ECIESLabel("nonce"), Length)
//
// Where ECIESLabel is specified as:
//
// struct {
//   uint16 length = Length;
//   opaque label<12..255> = "mls10 ecies " + Label;
// } ECIESLabel;
struct ECIESLabel
{
  uint16_t length;
  tls::opaque<1, 12> label;
};

tls::ostream&
operator<<(tls::ostream& out, const ECIESLabel& obj)
{
  return out << obj.length << obj.label;
}

static std::pair<bytes, bytes>
derive_ecies_secrets(const bytes& shared_secret)
{
  std::string key_label_str{ "mls10 ecies key" };
  bytes key_label_vec{ key_label_str.begin(), key_label_str.end() };
  HKDFLabel key_label{ AESGCM::key_size_128, key_label_vec };
  auto key = hkdf_expand(shared_secret, key_label, AESGCM::key_size_128);

  std::string nonce_label_str{ "mls10 ecies nonce" };
  bytes nonce_label_vec{ nonce_label_str.begin(), nonce_label_str.end() };
  HKDFLabel nonce_label{ AESGCM::nonce_size, nonce_label_vec };
  auto nonce = hkdf_expand(shared_secret, nonce_label, AESGCM::nonce_size);

  return std::pair<bytes, bytes>(key, nonce);
}

ECIESCiphertext
DHPublicKey::encrypt(const bytes& plaintext) const
{
  auto ephemeral = DHPrivateKey::generate();
  auto shared_secret = ephemeral.derive(*this);

  bytes key, nonce;
  std::tie(key, nonce) = derive_ecies_secrets(shared_secret);

  AESGCM gcm(key, nonce);
  auto content = gcm.encrypt(plaintext);
  return ECIESCiphertext{ ephemeral.public_key(), content };
}

tls::ostream&
operator<<(tls::ostream& out, const DHPublicKey& obj)
{
  tls::vector<uint8_t, 2> data = obj.to_bytes();
  return out << data;
}

tls::istream&
operator>>(tls::istream& in, DHPublicKey& obj)
{
  tls::vector<uint8_t, 2> data;
  in >> data;
  obj.reset(data);
  return in;
}

///
/// DHPrivateKey
///

DHPrivateKey
DHPrivateKey::generate()
{
  DHPrivateKey key;
  key._key.reset(OpenSSLKey::create(DH_KEY_TYPE));
  key._key->generate();
  key._pub._key.reset(key._key->dup_public());
  return key;
}

DHPrivateKey
DHPrivateKey::derive(const bytes& seed)
{
  DHPrivateKey key;
  key._key.reset(OpenSSLKey::create(DH_KEY_TYPE));
  key._key->set_secret(seed);
  key._pub._key.reset(key._key->dup_public());
  return key;
}

DHPrivateKey::DHPrivateKey(const DHPrivateKey& other)
  : _key(other._key->dup())
  , _pub(other._pub)
{}

DHPrivateKey::DHPrivateKey(DHPrivateKey&& other)
  : _key(std::move(other._key))
  , _pub(std::move(other._pub))
{}

DHPrivateKey&
DHPrivateKey::operator=(const DHPrivateKey& other)
{
  if (this != &other) {
    _key.reset(other._key->dup());
    _pub = other._pub;
  }
  return *this;
}

DHPrivateKey&
DHPrivateKey::operator=(DHPrivateKey&& other)
{
  if (this != &other) {
    _key = std::move(other._key);
    _pub = std::move(other._pub);
  }
  return *this;
}

bool
DHPrivateKey::operator==(const DHPrivateKey& other) const
{
  return *_key == *other._key;
}

bool
DHPrivateKey::operator!=(const DHPrivateKey& other) const
{
  return !(*this == other);
}

bytes
DHPrivateKey::derive(const DHPublicKey& pub) const
{
  return _key->derive(*pub._key);
}

const DHPublicKey&
DHPrivateKey::public_key() const
{
  return _pub;
}

bytes
DHPrivateKey::decrypt(const ECIESCiphertext& ciphertext) const
{
  auto shared_secret = derive(ciphertext.ephemeral);

  bytes key, nonce;
  std::tie(key, nonce) = derive_ecies_secrets(shared_secret);

  AESGCM gcm(key, nonce);
  return gcm.decrypt(ciphertext.content);
}

///
/// ECIESCiphertext
///

tls::ostream&
operator<<(tls::ostream& out, const ECIESCiphertext& obj)
{
  return out << obj.ephemeral << obj.content;
}

tls::istream&
operator>>(tls::istream& in, ECIESCiphertext& obj)
{
  return in >> obj.ephemeral >> obj.content;
}

///
/// SignaturePublicKey
///

SignaturePublicKey::SignaturePublicKey()
  : _key(OpenSSLKey::create(SIG_KEY_TYPE))
{}

SignaturePublicKey::SignaturePublicKey(const SignaturePublicKey& other)
  : _key(other._key->dup_public())
{}

SignaturePublicKey::SignaturePublicKey(SignaturePublicKey&& other)
  : _key(std::move(other._key))
{}

SignaturePublicKey::SignaturePublicKey(const bytes& data)
  : _key(OpenSSLKey::create(SIG_KEY_TYPE))
{
  reset(data);
}

SignaturePublicKey&
SignaturePublicKey::operator=(const SignaturePublicKey& other)
{
  if (&other != this) {
    _key.reset(other._key->dup_public());
  }
  return *this;
}

SignaturePublicKey&
SignaturePublicKey::operator=(SignaturePublicKey&& other)
{
  if (&other != this) {
    _key = std::move(other._key);
  }
  return *this;
}

bool
SignaturePublicKey::operator==(const SignaturePublicKey& other) const
{
  return *_key == *other._key;
}

bool
SignaturePublicKey::operator!=(const SignaturePublicKey& other) const
{
  return !(*this == other);
}

bool
SignaturePublicKey::verify(const bytes& message, const bytes& signature) const
{
  return _key->verify(message, signature);
}

bytes
SignaturePublicKey::to_bytes() const
{
  return _key->marshal();
}

void
SignaturePublicKey::reset(const bytes& data)
{
  _key->set_public(data);
}

tls::ostream&
operator<<(tls::ostream& out, const SignaturePublicKey& obj)
{
  tls::vector<uint8_t, 2> data = obj.to_bytes();
  return out << data;
}

tls::istream&
operator>>(tls::istream& in, SignaturePublicKey& obj)
{
  tls::vector<uint8_t, 2> data;
  in >> data;
  obj.reset(data);
  return in;
}

///
/// SignaturePrivateKey
///

SignaturePrivateKey
SignaturePrivateKey::generate()
{
  SignaturePrivateKey key;
  key._key.reset(OpenSSLKey::create(SIG_KEY_TYPE));
  key._key->generate();
  key._pub._key.reset(key._key->dup_public());
  return key;
}

SignaturePrivateKey::SignaturePrivateKey(const SignaturePrivateKey& other)
  : _key(other._key->dup())
  , _pub(other._pub)
{}

SignaturePrivateKey::SignaturePrivateKey(SignaturePrivateKey&& other)
  : _key(std::move(other._key))
  , _pub(std::move(other._pub))
{}

SignaturePrivateKey&
SignaturePrivateKey::operator=(const SignaturePrivateKey& other)
{
  if (this != &other) {
    _key.reset(other._key->dup());
    _pub = other._pub;
  }
  return *this;
}

SignaturePrivateKey&
SignaturePrivateKey::operator=(SignaturePrivateKey&& other)
{
  if (this != &other) {
    _key = std::move(other._key);
    _pub = std::move(other._pub);
  }
  return *this;
}

bool
SignaturePrivateKey::operator==(const SignaturePrivateKey& other) const
{
  return *_key == *other._key;
}

bool
SignaturePrivateKey::operator!=(const SignaturePrivateKey& other) const
{
  return !(*this == other);
}

bytes
SignaturePrivateKey::sign(const bytes& message) const
{
  return _key->sign(message);
}

const SignaturePublicKey&
SignaturePrivateKey::public_key() const
{
  return _pub;
}

} // namespace mls
