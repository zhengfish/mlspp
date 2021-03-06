#include "messages.h"
#include "test_vectors.h"
#include "tls_syntax.h"
#include <gtest/gtest.h>

using namespace mls;

template<typename T>
void
tls_round_trip(const bytes& vector,
               T& constructed,
               T& unmarshaled,
               bool reproducible)
{
  auto marshaled = tls::marshal(constructed);
  if (reproducible) {
    ASSERT_EQ(vector, marshaled);
  }

  tls::unmarshal(vector, unmarshaled);
  ASSERT_EQ(constructed, unmarshaled);
  ASSERT_EQ(tls::marshal(unmarshaled), vector);
}

class MessagesTest : public ::testing::Test
{
protected:
  const MessagesTestVectors& tv;

  MessagesTest()
    : tv(TestLoader<MessagesTestVectors>::get())
  {}

  void tls_round_trip_all(const MessagesTestVectors::TestCase& tc,
                          bool reproducible)
  {
    // Miscellaneous data items we need to construct messages
    auto dh_priv = DHPrivateKey::derive(tc.cipher_suite, tv.dh_seed);
    auto dh_key = dh_priv.public_key();
    auto sig_priv = SignaturePrivateKey::derive(tc.sig_scheme, tv.sig_seed);
    auto sig_key = sig_priv.public_key();

    mls::test::DeterministicHPKE lock;
    auto ratchet_tree =
      RatchetTree{ tc.cipher_suite,
                   { tv.random, tv.random, tv.random, tv.random } };
    ratchet_tree.blank_path(LeafIndex{ 2 });
    auto direct_path = ratchet_tree.encrypt(LeafIndex{ 0 }, tv.random);

    auto cred = Credential::basic(tv.user_id, sig_key);
    auto roster = Roster{};
    roster.add(0, cred);

    // UserInitKey
    UserInitKey user_init_key_c;
    user_init_key_c.user_init_key_id = tv.uik_id;
    user_init_key_c.add_init_key(dh_key);
    user_init_key_c.credential = cred;
    user_init_key_c.signature = tv.random;

    UserInitKey user_init_key;
    tls_round_trip(
      tc.user_init_key, user_init_key_c, user_init_key, reproducible);

    // WelcomeInfo and Welcome
    WelcomeInfo welcome_info_c{
      tv.group_id, tv.epoch, roster, ratchet_tree, tv.random, tv.random,
    };
    Welcome welcome_c{ tv.uik_id, dh_key, welcome_info_c };

    WelcomeInfo welcome_info{ tc.cipher_suite };
    tls_round_trip(tc.welcome_info, welcome_info_c, welcome_info, true);

    Welcome welcome;
    tls_round_trip(tc.welcome, welcome_c, welcome, true);

    // Handshake messages
    Add add_op{ tv.removed, user_init_key_c, tv.random };
    Update update_op{ direct_path };
    Remove remove_op{ tv.removed, direct_path };

    Handshake add_c{ tv.epoch, add_op, tv.signer_index, tv.random, tv.random };
    Handshake update_c{
      tv.epoch, update_op, tv.signer_index, tv.random, tv.random
    };
    Handshake remove_c{
      tv.epoch, remove_op, tv.signer_index, tv.random, tv.random
    };

    Handshake add{ tc.cipher_suite };
    tls_round_trip(tc.add, add_c, add, reproducible);

    Handshake update{ tc.cipher_suite };
    tls_round_trip(tc.update, update_c, update, true);

    Handshake remove{ tc.cipher_suite };
    tls_round_trip(tc.remove, remove_c, remove, true);
  }
};

TEST_F(MessagesTest, UserInitKey)
{
  std::vector<CipherSuite> suites{
    CipherSuite::P256_SHA256_AES128GCM,
    CipherSuite::X25519_SHA256_AES128GCM,
  };

  UserInitKey constructed;
  constructed.user_init_key_id = tv.uik_id;
  for (const auto& suite : suites) {
    auto priv = DHPrivateKey::derive(suite, tv.dh_seed);
    constructed.add_init_key(priv.public_key());
  }

  auto identity_priv =
    SignaturePrivateKey::derive(tv.uik_all_scheme, tv.sig_seed);
  constructed.credential = Credential::basic(tv.user_id, identity_priv);
  constructed.signature = tv.random;

  UserInitKey after;
  auto reproducible =
    mls::test::deterministic_signature_scheme(tv.uik_all_scheme);
  tls_round_trip(tv.user_init_key_all, constructed, after, reproducible);
}

TEST_F(MessagesTest, Suite_P256_P256)
{
  tls_round_trip_all(tv.case_p256_p256, false);
}

TEST_F(MessagesTest, Suite_X25519_Ed25519)
{
  tls_round_trip_all(tv.case_x25519_ed25519, true);
}
