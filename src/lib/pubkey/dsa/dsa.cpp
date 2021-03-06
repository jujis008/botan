/*
* DSA
* (C) 1999-2010,2014 Jack Lloyd
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include <botan/internal/pk_utils.h>
#include <botan/dsa.h>
#include <botan/keypair.h>
#include <botan/pow_mod.h>
#include <botan/reducer.h>
#include <botan/rfc6979.h>
#include <future>

namespace Botan {

/*
* DSA_PublicKey Constructor
*/
DSA_PublicKey::DSA_PublicKey(const DL_Group& grp, const BigInt& y1)
   {
   group = grp;
   y = y1;
   }

/*
* Create a DSA private key
*/
DSA_PrivateKey::DSA_PrivateKey(RandomNumberGenerator& rng,
                               const DL_Group& grp,
                               const BigInt& x_arg)
   {
   group = grp;
   x = x_arg;

   if(x == 0)
      x = BigInt::random_integer(rng, 2, group_q() - 1);

   y = power_mod(group_g(), x, group_p());

   if(x_arg == 0)
      gen_check(rng);
   else
      load_check(rng);
   }

DSA_PrivateKey::DSA_PrivateKey(const AlgorithmIdentifier& alg_id,
                               const secure_vector<byte>& key_bits,
                               RandomNumberGenerator& rng) :
   DL_Scheme_PrivateKey(alg_id, key_bits, DL_Group::ANSI_X9_57)
   {
   y = power_mod(group_g(), x, group_p());

   load_check(rng);
   }

/*
* Check Private DSA Parameters
*/
bool DSA_PrivateKey::check_key(RandomNumberGenerator& rng, bool strong) const
   {
   if(!DL_Scheme_PrivateKey::check_key(rng, strong) || x >= group_q())
      return false;

   if(!strong)
      return true;

   return KeyPair::signature_consistency_check(rng, *this, "EMSA1(SHA-1)");
   }

namespace {

/**
* Object that can create a DSA signature
*/
class DSA_Signature_Operation : public PK_Ops::Signature_with_EMSA
   {
   public:
      typedef DSA_PrivateKey Key_Type;
      DSA_Signature_Operation(const DSA_PrivateKey& dsa, const std::string& emsa) :
         PK_Ops::Signature_with_EMSA(emsa),
         q(dsa.group_q()),
         x(dsa.get_x()),
         powermod_g_p(dsa.group_g(), dsa.group_p()),
         mod_q(dsa.group_q()),
         m_hash(hash_for_deterministic_signature(emsa))
         {
         }

      size_t message_parts() const override { return 2; }
      size_t message_part_size() const override { return q.bytes(); }
      size_t max_input_bits() const override { return q.bits(); }

      secure_vector<byte> raw_sign(const byte msg[], size_t msg_len,
                                   RandomNumberGenerator& rng) override;
   private:
      const BigInt& q;
      const BigInt& x;
      Fixed_Base_Power_Mod powermod_g_p;
      Modular_Reducer mod_q;
      std::string m_hash;
   };

secure_vector<byte>
DSA_Signature_Operation::raw_sign(const byte msg[], size_t msg_len,
                                  RandomNumberGenerator&)
   {
   BigInt i(msg, msg_len);

   while(i >= q)
      i -= q;

   const BigInt k = generate_rfc6979_nonce(x, q, i, m_hash);

   auto future_r = std::async(std::launch::async,
                              [&]() { return mod_q.reduce(powermod_g_p(k)); });

   BigInt s = inverse_mod(k, q);
   const BigInt r = future_r.get();
   s = mod_q.multiply(s, mul_add(x, r, i));

   // With overwhelming probability, a bug rather than actual zero r/s
   BOTAN_ASSERT(s != 0, "invalid s");
   BOTAN_ASSERT(r != 0, "invalid r");

   secure_vector<byte> output(2*q.bytes());
   r.binary_encode(&output[output.size() / 2 - r.bytes()]);
   s.binary_encode(&output[output.size() - s.bytes()]);
   return output;
   }

/**
* Object that can verify a DSA signature
*/
class DSA_Verification_Operation : public PK_Ops::Verification_with_EMSA
   {
   public:
      typedef DSA_PublicKey Key_Type;
      DSA_Verification_Operation(const DSA_PublicKey& dsa,
                                 const std::string& emsa) :
         PK_Ops::Verification_with_EMSA(emsa),
         q(dsa.group_q()), y(dsa.get_y())
         {
         powermod_g_p = Fixed_Base_Power_Mod(dsa.group_g(), dsa.group_p());
         powermod_y_p = Fixed_Base_Power_Mod(y, dsa.group_p());
         mod_p = Modular_Reducer(dsa.group_p());
         mod_q = Modular_Reducer(dsa.group_q());
         }

      size_t message_parts() const { return 2; }
      size_t message_part_size() const { return q.bytes(); }
      size_t max_input_bits() const { return q.bits(); }

      bool with_recovery() const override { return false; }

      bool verify(const byte msg[], size_t msg_len,
                  const byte sig[], size_t sig_len) override;
   private:
      const BigInt& q;
      const BigInt& y;

      Fixed_Base_Power_Mod powermod_g_p, powermod_y_p;
      Modular_Reducer mod_p, mod_q;
   };

bool DSA_Verification_Operation::verify(const byte msg[], size_t msg_len,
                                        const byte sig[], size_t sig_len)
   {
   if(sig_len != 2*q.bytes() || msg_len > q.bytes())
      return false;

   BigInt r(sig, q.bytes());
   BigInt s(sig + q.bytes(), q.bytes());
   BigInt i(msg, msg_len);

   if(r <= 0 || r >= q || s <= 0 || s >= q)
      return false;

   s = inverse_mod(s, q);

   auto future_s_i = std::async(std::launch::async,
      [&]() { return powermod_g_p(mod_q.multiply(s, i)); });

   BigInt s_r = powermod_y_p(mod_q.multiply(s, r));
   BigInt s_i = future_s_i.get();

   s = mod_p.multiply(s_i, s_r);

   return (mod_q.reduce(s) == r);
   }

BOTAN_REGISTER_PK_SIGNATURE_OP("DSA", DSA_Signature_Operation);
BOTAN_REGISTER_PK_VERIFY_OP("DSA", DSA_Verification_Operation);

}

}
