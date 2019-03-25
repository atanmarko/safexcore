// Copyright (c) 2018, The Safex Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers
// Parts of this file are originally copyright (c) 2014-2018 The Monero Project

#include <unordered_set>
#include <random>
#include "include_base_utils.h"
#include "string_tools.h"
using namespace epee;

#include "common/apply_permutation.h"
#include "cryptonote_tx_utils.h"
#include "cryptonote_config.h"
#include "cryptonote_basic/miner.h"
#include "crypto/crypto.h"
#include "crypto/hash.h"
#include "ringct/rctSigs.h"

using namespace crypto;

namespace cryptonote
{
  //---------------------------------------------------------------
  void classify_addresses(const std::vector<tx_destination_entry> &destinations, const boost::optional<cryptonote::account_public_address>& change_addr, size_t &num_stdaddresses, size_t &num_subaddresses, account_public_address &single_dest_subaddress)
  {
    num_stdaddresses = 0;
    num_subaddresses = 0;
    std::unordered_set<cryptonote::account_public_address> unique_dst_addresses;
    for(const tx_destination_entry& dst_entr: destinations)
    {
      if (change_addr && dst_entr.addr == change_addr)
        continue;
      if (unique_dst_addresses.count(dst_entr.addr) == 0)
      {
        unique_dst_addresses.insert(dst_entr.addr);
        if (dst_entr.is_subaddress)
        {
          ++num_subaddresses;
          single_dest_subaddress = dst_entr.addr;
        }
        else
        {
          ++num_stdaddresses;
        }
      }
    }
    LOG_PRINT_L2("destinations include " << num_stdaddresses << " standard addresses and " << num_subaddresses << " subaddresses");
  }
  //---------------------------------------------------------------
  bool construct_miner_tx(size_t height, size_t median_size, uint64_t already_generated_coins, size_t current_block_size, uint64_t fee, const account_public_address &miner_address, transaction& tx, const blobdata& extra_nonce, size_t max_outs, uint8_t hard_fork_version) {
    tx.vin.clear();
    tx.vout.clear();
    tx.extra.clear();

    keypair txkey = keypair::generate(hw::get_device("default"));
    add_tx_pub_key_to_extra(tx, txkey.pub);
    if(!extra_nonce.empty())
      if(!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
        return false;

    txin_gen in;
    in.height = height;

    uint64_t block_reward;
    if(!get_block_reward(median_size, current_block_size, already_generated_coins, block_reward, hard_fork_version, height))
    {
      LOG_PRINT_L0("Block is too big");
      return false;
    }

#if defined(DEBUG_CREATE_BLOCK_TEMPLATE)
    LOG_PRINT_L1("Creating block template: reward " << block_reward <<
      ", fee " << fee);
#endif
    block_reward += fee;

    // from hard fork 2, we cut out the low significant digits. This makes the tx smaller, and
    // keeps the paid amount almost the same. The unpaid remainder gets pushed back to the
    // emission schedule
    // from hard fork 4, we use a single "dusty" output. This makes the tx even smaller,
    // and avoids the quantization. These outputs will be added as rct outputs with identity
    // masks, to they can be used as rct inputs.
    if (hard_fork_version >= 2 && hard_fork_version < 4) {
      block_reward = block_reward - block_reward % ::config::BASE_REWARD_CLAMP_THRESHOLD;
    }

    //decompose_offset purpose is to force decomposition of block reward
    //if base_reward is rounded to whole number of coins
    const uint64_t decompose_offset = ::config::BASE_REWARD_DECOMPOSITION_OFFSET;
    std::vector<uint64_t> out_amounts;
    decompose_amount_into_digits(block_reward-decompose_offset, hard_fork_version >= 2 ? 0 : ::config::DEFAULT_DUST_THRESHOLD,
      [&out_amounts](uint64_t a_chunk) { out_amounts.push_back(a_chunk); },
      [&out_amounts](uint64_t a_dust) { out_amounts.push_back(a_dust); });

    if (decompose_offset > 0 && out_amounts.size() > 0) {
    	out_amounts[0]+=decompose_offset;
    }

    CHECK_AND_ASSERT_MES(1 <= max_outs, false, "max_out must be non-zero");
    if (height == 0 || hard_fork_version >= HF_VERSION_ENFORCE_RCT)
    {
      // the genesis block was not decomposed, for unknown reasons
      while (max_outs < out_amounts.size())
      {
        out_amounts[1] += out_amounts[0];
        for (size_t n = 1; n < out_amounts.size(); ++n)
          out_amounts[n - 1] = out_amounts[n];
        out_amounts.pop_back();
      }
    }
    else
    {
      CHECK_AND_ASSERT_MES(max_outs >= out_amounts.size(), false, "max_out exceeded");
    }

    uint64_t summary_amounts = 0;
    for (size_t no = 0; no < out_amounts.size(); no++)
    {
      crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);;
      crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);
      bool r = crypto::generate_key_derivation(miner_address.m_view_public_key, txkey.sec, derivation);
      CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to generate_key_derivation(" << miner_address.m_view_public_key << ", " << txkey.sec << ")");

      r = crypto::derive_public_key(derivation, no, miner_address.m_spend_public_key, out_eph_public_key);
      CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to derive_public_key(" << derivation << ", " << no << ", "<< miner_address.m_spend_public_key << ")");

      txout_to_key tk;
      tk.key = out_eph_public_key;

      tx_out out;
      summary_amounts += out.amount = out_amounts[no];
      out.target = tk;
      tx.vout.push_back(out);
    }

    CHECK_AND_ASSERT_MES(summary_amounts == block_reward, false, "Failed to construct miner tx, summary_amounts = " << summary_amounts << " not equal block_reward = " << block_reward);

    //Currently we will only use version 1, version 2 is tbd
    tx.version = 1;

    //lock
    tx.unlock_time = height + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW;
    tx.vin.push_back(in);

    tx.invalidate_hashes();

    //LOG_PRINT("MINER_TX generated ok, block_reward=" << print_money(block_reward) << "("  << print_money(block_reward - fee) << "+" << print_money(fee)
    //  << "), current_block_size=" << current_block_size << ", already_generated_coins=" << already_generated_coins << ", tx_id=" << get_transaction_hash(tx)
    // << ", height=" << height , LOG_LEVEL_2);
    return true;
  }
  //---------------------------------------------------------------
  crypto::public_key get_destination_view_key_pub(const std::vector<tx_destination_entry> &destinations, const boost::optional<cryptonote::account_public_address>& change_addr)
  {
    account_public_address addr = {null_pkey, null_pkey};
    size_t count = 0;
    for (const auto &i : destinations)
    {
      if ((i.amount == 0) && (i.token_amount == 0))
        continue;
      if (change_addr && i.addr == *change_addr)
        continue;
      if (i.addr == addr)
        continue;
      if (count > 0)
        return null_pkey;
      addr = i.addr;
      ++count;
    }
    if (count == 0 && change_addr)
      return change_addr->m_view_public_key;
    return addr.m_view_public_key;
  }
  //---------------------------------------------------------------
  bool construct_tx_with_tx_key(const account_keys& sender_account_keys, const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses,
          std::vector<tx_source_entry>& sources, std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::account_public_address>& change_addr,
          std::vector<uint8_t> extra, transaction& tx, uint64_t unlock_time, const crypto::secret_key &tx_key,
          const std::vector<crypto::secret_key> &additional_tx_keys, bool shuffle_outs)
  {
    hw::device &hwdev = sender_account_keys.get_device();

    if (sources.empty())
    {
      LOG_ERROR("Empty sources");
      return false;
    }

    std::vector<rct::key> amount_keys;
    tx.set_null();
    amount_keys.clear();

    tx.version = 1;
    tx.unlock_time = unlock_time;

    tx.extra = extra;
    crypto::public_key txkey_pub = AUTO_VAL_INIT(txkey_pub);

    // if we have a stealth payment id, find it and encrypt it with the tx key now
    boost::optional<crypto::hash> bitcoin_transaction_hash{};
    std::vector<tx_extra_field> tx_extra_fields;
    if (parse_tx_extra(tx.extra, tx_extra_fields))
    {
      tx_extra_nonce extra_nonce = AUTO_VAL_INIT(extra_nonce);
      if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
      {
        crypto::hash8 payment_id = null_hash8;
        if (get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
        {
          LOG_PRINT_L2("Encrypting payment id " << payment_id);
          crypto::public_key view_key_pub = get_destination_view_key_pub(destinations, change_addr);
          if (view_key_pub == null_pkey)
          {
            LOG_ERROR("Destinations have to have exactly one output to support encrypted payment ids");
            return false;
          }

          if (!hwdev.encrypt_payment_id(payment_id, view_key_pub, tx_key))
          {
            LOG_ERROR("Failed to encrypt payment id");
            return false;
          }

          std::string extra_nonce;
          set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id);
          remove_field_from_tx_extra(tx.extra, typeid(tx_extra_nonce));
          if (!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
          {
            LOG_ERROR("Failed to add encrypted payment id to tx extra");
            return false;
          }
          LOG_PRINT_L1("Encrypted payment ID: " << payment_id);
        }

      }

      tx_extra_bitcoin_hash bitcoin_hash_extra_data = AUTO_VAL_INIT(bitcoin_hash_extra_data);
      if (find_tx_extra_field_by_type(tx_extra_fields, bitcoin_hash_extra_data))
      {
        bitcoin_transaction_hash = bitcoin_hash_extra_data.bitcoin_hash;
        remove_field_from_tx_extra(tx.extra, typeid(bitcoin_hash_extra_data));
      }


    }
    else
    {
      LOG_ERROR("Failed to parse tx extra");
      return false;
    }

    struct input_generation_context_data
    {
      keypair in_ephemeral = AUTO_VAL_INIT(in_ephemeral);
    };
    std::vector<input_generation_context_data> in_contexts;

    uint64_t summary_inputs_money = 0;
    uint64_t summary_inputs_tokens = 0;
    //fill inputs
    int idx = -1;
    for(const tx_source_entry &src_entr : sources)
    {
      ++idx;
      const bool migration_input = (src_entr.referenced_output_type == tx_out_type::out_bitcoin_migration);
      const bool token_transaction = (src_entr.referenced_output_type == tx_out_type::out_token) || (src_entr.referenced_output_type == tx_out_type::out_bitcoin_migration);
      if (migration_input)
      {
        txin_token_migration input_token_migration = AUTO_VAL_INIT(input_token_migration);
        input_token_migration.token_amount = src_entr.token_amount;
        CHECK_AND_ASSERT_MES(bitcoin_transaction_hash, false, "no bitcoin transaction hash for token migration input");
        memcpy(input_token_migration.bitcoin_burn_transaction.data, bitcoin_transaction_hash->data, sizeof(crypto::hash));
        generate_migration_key_image(*bitcoin_transaction_hash, input_token_migration.k_image);
        summary_inputs_tokens += src_entr.token_amount;
        tx.vin.emplace_back(input_token_migration);

        //add dummy ephemeral for sort function to work
        in_contexts.push_back(input_generation_context_data());
      }
      else
      {

        if (src_entr.real_output >= src_entr.outputs.size())
        {
          LOG_ERROR("real_output index (" << src_entr.real_output << ")bigger than output_keys.size()=" << src_entr.outputs.size());
          return false;
        }
        summary_inputs_money += src_entr.amount;
        summary_inputs_tokens += src_entr.token_amount;

        //key_derivation recv_derivation;
        in_contexts.push_back(input_generation_context_data());
        keypair& in_ephemeral = in_contexts.back().in_ephemeral;
        crypto::key_image img;
        const auto& out_key = reinterpret_cast<const crypto::public_key&>(src_entr.outputs[src_entr.real_output].second.dest);
        if(!generate_key_image_helper(sender_account_keys, subaddresses, out_key, src_entr.real_out_tx_key, src_entr.real_out_additional_tx_keys, src_entr.real_output_in_tx_index, in_ephemeral,img, hwdev))
        {
          LOG_ERROR("Key image generation failed!");
          return false;
        }

        //check that derivated key is equal with real output key
        if(!migration_input && !(in_ephemeral.pub == src_entr.outputs[src_entr.real_output].second.dest) )
        {
          LOG_ERROR("derived public key mismatch with output public key at index " << idx << ", real out " << src_entr.real_output << "! "<< ENDL << "derived_key:"
              << string_tools::pod_to_hex(in_ephemeral.pub) << ENDL << "real output_public_key:"
              << string_tools::pod_to_hex(src_entr.outputs[src_entr.real_output].second.dest) );
          LOG_ERROR("token_amount " << src_entr.token_amount << ", amount " << src_entr.amount);
          LOG_ERROR("tx pubkey " << src_entr.real_out_tx_key << ", real_output_in_tx_index " << src_entr.real_output_in_tx_index);
          return false;
        }

        if (token_transaction)
        {
          txin_token_to_key input_token_to_key = AUTO_VAL_INIT(input_token_to_key);
          input_token_to_key.token_amount = src_entr.token_amount;
          input_token_to_key.k_image = img;

          //fill outputs array and use relative offsets
          for(const tx_source_entry::output_entry& out_entry: src_entr.outputs)
            input_token_to_key.key_offsets.push_back(out_entry.first);

          input_token_to_key.key_offsets = absolute_output_offsets_to_relative(input_token_to_key.key_offsets);
          tx.vin.push_back(input_token_to_key);
        }
        else
        {
          //put key image into tx input
          txin_to_key input_to_key = AUTO_VAL_INIT(input_to_key);
          input_to_key.amount = src_entr.amount;
          input_to_key.k_image = img;

          //fill outputs array and use relative offsets
          for(const tx_source_entry::output_entry& out_entry: src_entr.outputs)
            input_to_key.key_offsets.push_back(out_entry.first);

          input_to_key.key_offsets = absolute_output_offsets_to_relative(input_to_key.key_offsets);
          tx.vin.push_back(input_to_key);
        }

      }
    }

    if (shuffle_outs)
    {
      std::shuffle(destinations.begin(), destinations.end(), std::default_random_engine(crypto::rand<unsigned int>()));
    }

    // sort ins by their key image
    std::vector<size_t> ins_order(sources.size());
    for (size_t n = 0; n < sources.size(); ++n)
      ins_order[n] = n;
    std::sort(ins_order.begin(), ins_order.end(), [&](const size_t i0, const size_t i1) {
      const crypto::key_image &tk0_key_image = *boost::apply_visitor(key_image_visitor(), tx.vin[i0]);
      const crypto::key_image &tk1_key_image = *boost::apply_visitor(key_image_visitor(), tx.vin[i1]);
      return memcmp(&tk0_key_image, &tk1_key_image, sizeof(tk1_key_image)) > 0;
    });
    tools::apply_permutation(ins_order, [&] (size_t i0, size_t i1) {
      std::swap(tx.vin[i0], tx.vin[i1]);
      std::swap(in_contexts[i0], in_contexts[i1]);
      std::swap(sources[i0], sources[i1]);
    });

    // figure out if we need to make additional tx pubkeys
    size_t num_stdaddresses = 0;
    size_t num_subaddresses = 0;
    account_public_address single_dest_subaddress = AUTO_VAL_INIT(single_dest_subaddress);
    classify_addresses(destinations, change_addr, num_stdaddresses, num_subaddresses, single_dest_subaddress);

    // if this is a single-destination transfer to a subaddress, we set the tx pubkey to R=s*D
    if (num_stdaddresses == 0 && num_subaddresses == 1)
    {
      txkey_pub = rct::rct2pk(hwdev.scalarmultKey(rct::pk2rct(single_dest_subaddress.m_spend_public_key), rct::sk2rct(tx_key)));
    }
    else
    {
      txkey_pub = rct::rct2pk(hwdev.scalarmultBase(rct::sk2rct(tx_key)));
    }
    remove_field_from_tx_extra(tx.extra, typeid(tx_extra_pub_key));
    add_tx_pub_key_to_extra(tx, txkey_pub);

    std::vector<crypto::public_key> additional_tx_public_keys;

    // we don't need to include additional tx keys if:
    //   - all the destinations are standard addresses
    //   - there's only one destination which is a subaddress
    bool need_additional_txkeys = num_subaddresses > 0 && (num_stdaddresses > 0 || num_subaddresses > 1);
    if (need_additional_txkeys)
      CHECK_AND_ASSERT_MES(destinations.size() == additional_tx_keys.size(), false, "Wrong amount of additional tx keys");

    uint64_t summary_outs_money = 0;
    uint64_t summary_outs_tokens = 0;
    //fill outputs
    size_t output_index = 0;
    for(const tx_destination_entry& dst_entr: destinations)
    {
      CHECK_AND_ASSERT_MES(dst_entr.amount > 0 || dst_entr.token_amount > 0 || tx.version > 1, false, "Destination with wrong amount: " << dst_entr.amount << " or token amount " << dst_entr.token_amount);
      crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
      crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);

      // make additional tx pubkey if necessary
      keypair additional_txkey = AUTO_VAL_INIT(additional_txkey);
      if (need_additional_txkeys)
      {
        additional_txkey.sec = additional_tx_keys[output_index];
        if (dst_entr.is_subaddress)
          additional_txkey.pub = rct::rct2pk(hwdev.scalarmultKey(rct::pk2rct(dst_entr.addr.m_spend_public_key), rct::sk2rct(additional_txkey.sec)));
        else
          additional_txkey.pub = rct::rct2pk(hwdev.scalarmultBase(rct::sk2rct(additional_txkey.sec)));
      }

      bool r;
      if (change_addr && dst_entr.addr == *change_addr)
      {
        // sending change to yourself; derivation = a*R
        r = hwdev.generate_key_derivation(txkey_pub, sender_account_keys.m_view_secret_key, derivation);
        CHECK_AND_ASSERT_MES(r, false, "at creation outs: failed to generate_key_derivation(" << txkey_pub << ", " << sender_account_keys.m_view_secret_key << ")");
      }
      else
      {
        // sending to the recipient; derivation = r*A (or s*C in the subaddress scheme)
        r = hwdev.generate_key_derivation(dst_entr.addr.m_view_public_key, dst_entr.is_subaddress && need_additional_txkeys ? additional_txkey.sec : tx_key, derivation);
        CHECK_AND_ASSERT_MES(r, false, "at creation outs: failed to generate_key_derivation(" << dst_entr.addr.m_view_public_key << ", " << (dst_entr.is_subaddress && need_additional_txkeys ? additional_txkey.sec : tx_key) << ")");
      }

      if (need_additional_txkeys)
      {
        additional_tx_public_keys.push_back(additional_txkey.pub);
      }

      r = hwdev.derive_public_key(derivation, output_index, dst_entr.addr.m_spend_public_key, out_eph_public_key);
      CHECK_AND_ASSERT_MES(r, false, "at creation outs: failed to derive_public_key(" << derivation << ", " << output_index << ", "<< dst_entr.addr.m_spend_public_key << ")");

      hwdev.add_output_key_mapping(dst_entr.addr.m_view_public_key, dst_entr.addr.m_spend_public_key, dst_entr.is_subaddress, output_index, amount_keys.back(), out_eph_public_key);

      tx_out out = AUTO_VAL_INIT(out);

      if (dst_entr.token_transaction) {
        out.token_amount = dst_entr.token_amount;
        out.amount = 0;
        txout_token_to_key ttk = AUTO_VAL_INIT(ttk);
        ttk.key = out_eph_public_key;
        out.target = ttk;
        tx.vout.push_back(out);
      } else {
        out.amount = dst_entr.amount;
        out.token_amount = 0;
        txout_to_key tk = AUTO_VAL_INIT(tk);
        tk.key = out_eph_public_key;
        out.target = tk;
        tx.vout.push_back(out);
      }

      output_index++;
      summary_outs_money += dst_entr.amount;
      summary_outs_tokens += dst_entr.token_amount;
    }
    CHECK_AND_ASSERT_MES(additional_tx_public_keys.size() == additional_tx_keys.size(), false, "Internal error creating additional public keys");

    remove_field_from_tx_extra(tx.extra, typeid(tx_extra_additional_pub_keys));

    LOG_PRINT_L2("tx pubkey: " << txkey_pub);
    if (need_additional_txkeys)
    {
      LOG_PRINT_L2("additional tx pubkeys: ");
      for (size_t i = 0; i < additional_tx_public_keys.size(); ++i)
        LOG_PRINT_L2(additional_tx_public_keys[i]);
      add_additional_tx_pub_keys_to_extra(tx.extra, additional_tx_public_keys);
    }

    //check money
    if(summary_outs_money > summary_inputs_money )
    {
      LOG_ERROR("Transaction inputs money ("<< summary_inputs_money << ") less than outputs money (" << summary_outs_money << ")");
      return false;
    }

    //check tokens
    if(summary_outs_tokens > summary_inputs_tokens )
    {
      LOG_ERROR("Transaction inputs tokens ("<< summary_inputs_tokens << ") less than outputs tokens (" << summary_outs_tokens << ")");
      return false;
    }

    // check for watch only wallet
    bool zero_secret_key = true;
    for (size_t i = 0; i < sizeof(sender_account_keys.m_spend_secret_key); ++i)
      zero_secret_key &= (sender_account_keys.m_spend_secret_key.data[i] == 0);
    if (zero_secret_key)
    {
      MDEBUG("Null secret key, skipping signatures");
    }

    if (tx.version == 1)
    {
      //generate ring signatures
      crypto::hash tx_prefix_hash = AUTO_VAL_INIT(tx_prefix_hash);
      get_transaction_prefix_hash(tx, tx_prefix_hash);

      std::stringstream ss_ring_s;
      size_t i = 0;
      for(const tx_source_entry& src_entr:  sources)
      {
        ss_ring_s << "pub_keys:" << ENDL;
        std::vector<const crypto::public_key*> keys_ptrs;
        std::vector<crypto::public_key> keys(src_entr.outputs.size());
        size_t ii = 0;

        for(const tx_source_entry::output_entry& o: src_entr.outputs)
        {
          keys[ii] = rct2pk(o.second.dest);
          keys_ptrs.push_back(&keys[ii]);
          ss_ring_s << o.second.dest << ENDL;
          ++ii;
        }
        tx.signatures.push_back(std::vector<crypto::signature>());
        std::vector<crypto::signature>& sigs = tx.signatures.back();
        sigs.resize(src_entr.outputs.size());
        if (!zero_secret_key) {
          const crypto::key_image &k_image = *boost::apply_visitor(key_image_visitor(), tx.vin[i]);
          if (src_entr.referenced_output_type == tx_out_type::out_bitcoin_migration) {
            public_key spend_public_key = AUTO_VAL_INIT(spend_public_key);
            CHECK_AND_ASSERT_MES(crypto::secret_key_to_public_key(sender_account_keys.m_spend_secret_key, spend_public_key), false, "Could not create public_key from private_key");
            crypto::generate_signature(tx_prefix_hash, spend_public_key, sender_account_keys.m_spend_secret_key, sigs[0]);
          } else {
            crypto::generate_ring_signature(tx_prefix_hash, k_image, keys_ptrs, in_contexts[i].in_ephemeral.sec, src_entr.real_output, sigs.data());
          }
        }
        ss_ring_s << "signatures:" << ENDL;
        std::for_each(sigs.begin(), sigs.end(), [&](const crypto::signature& s){ss_ring_s << s << ENDL;});
        ss_ring_s << "prefix_hash:" << tx_prefix_hash << ENDL << "in_ephemeral_key: " << in_contexts[i].in_ephemeral.sec << ENDL << "real_output: " << src_entr.real_output << ENDL;
        i++;
      }

      MCINFO("construct_tx", "transaction_created: " << get_transaction_hash(tx) << ENDL << obj_to_json_str(tx) << ENDL << ss_ring_s.str());
    }
    else
    {
      LOG_ERROR("Transaction version>=2 not supported");
      return false;

    }

    tx.invalidate_hashes();

    return true;
  }
  //---------------------------------------------------------------
  bool construct_advanced_tx_with_tx_key(const account_keys& sender_account_keys, const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses,
          std::vector<tx_source_entry>& sources, std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::account_public_address>& change_addr,
          std::vector<uint8_t> extra, transaction& tx, uint64_t unlock_time, const crypto::secret_key &tx_key,
          const std::vector<crypto::secret_key> &additional_tx_keys, bool shuffle_outs)
  {
    hw::device &hwdev = sender_account_keys.get_device();

    if (sources.empty())
    {
      LOG_ERROR("Empty sources");
      return false;
    }

    std::vector<rct::key> amount_keys;
    tx.set_null();
    amount_keys.clear();

    tx.version = 2;
    tx.unlock_time = unlock_time;

    tx.extra = extra;
    crypto::public_key txkey_pub = AUTO_VAL_INIT(txkey_pub);

    // if we have a stealth payment id, find it and encrypt it with the tx key now
    std::vector<tx_extra_field> tx_extra_fields;
    if (parse_tx_extra(tx.extra, tx_extra_fields))
    {
      tx_extra_nonce extra_nonce = AUTO_VAL_INIT(extra_nonce);
      if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
      {
        crypto::hash8 payment_id = null_hash8;
        if (get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
        {
          LOG_PRINT_L2("Encrypting payment id " << payment_id);
          crypto::public_key view_key_pub = get_destination_view_key_pub(destinations, change_addr);
          if (view_key_pub == null_pkey)
          {
            LOG_ERROR("Destinations have to have exactly one output to support encrypted payment ids");
            return false;
          }

          if (!hwdev.encrypt_payment_id(payment_id, view_key_pub, tx_key))
          {
            LOG_ERROR("Failed to encrypt payment id");
            return false;
          }

          std::string extra_nonce;
          set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id);
          remove_field_from_tx_extra(tx.extra, typeid(tx_extra_nonce));
          if (!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
          {
            LOG_ERROR("Failed to add encrypted payment id to tx extra");
            return false;
          }
          LOG_PRINT_L1("Encrypted payment ID: " << payment_id);
        }

      }
    }
    else
    {
      LOG_ERROR("Failed to parse tx extra");
      return false;
    }

    struct input_generation_context_data
    {
      keypair in_ephemeral = AUTO_VAL_INIT(in_ephemeral);
    };
    std::vector<input_generation_context_data> in_contexts;

    uint64_t summary_inputs_money = 0;
    uint64_t summary_inputs_tokens = 0;

    //fill inputs
    int idx = -1;
    for(const tx_source_entry &src_entr : sources)
    {
      ++idx;

      if (src_entr.real_output >= src_entr.outputs.size())
      {
        LOG_ERROR("real_output index (" << src_entr.real_output << ")bigger than output_keys.size()=" << src_entr.outputs.size());
        return false;
      }
      summary_inputs_money += src_entr.amount;
      summary_inputs_tokens += src_entr.token_amount;

      //key_derivation recv_derivation;
      in_contexts.push_back(input_generation_context_data());
      keypair &in_ephemeral = in_contexts.back().in_ephemeral;
      crypto::key_image img;
      const auto &out_key = reinterpret_cast<const crypto::public_key &>(src_entr.outputs[src_entr.real_output].second.dest);
      if (!generate_key_image_helper(sender_account_keys, subaddresses, out_key, src_entr.real_out_tx_key, src_entr.real_out_additional_tx_keys, src_entr.real_output_in_tx_index, in_ephemeral, img, hwdev))
      {
        LOG_ERROR("Key image generation failed!");
        return false;
      }

      //check that derivated key is equal with real output key
      if (!(in_ephemeral.pub == src_entr.outputs[src_entr.real_output].second.dest))
      {
        LOG_ERROR("derived public key mismatch with output public key at index " << idx << ", real out " << src_entr.real_output << "! " << ENDL << "derived_key:"
                                                                                 << string_tools::pod_to_hex(in_ephemeral.pub) << ENDL << "real output_public_key:"
                                                                                 << string_tools::pod_to_hex(src_entr.outputs[src_entr.real_output].second.dest));
        LOG_ERROR("token_amount " << src_entr.token_amount << ", amount " << src_entr.amount);
        LOG_ERROR("tx pubkey " << src_entr.real_out_tx_key << ", real_output_in_tx_index " << src_entr.real_output_in_tx_index);
        return false;
      }

      if (src_entr.referenced_output_type == tx_out_type::out_token)
      {
        txin_token_to_key input_token_to_key = AUTO_VAL_INIT(input_token_to_key);
        input_token_to_key.token_amount = src_entr.token_amount;
        input_token_to_key.k_image = img;

        //fill outputs array and use relative offsets
        for (const tx_source_entry::output_entry &out_entry: src_entr.outputs)
          input_token_to_key.key_offsets.push_back(out_entry.first);

        input_token_to_key.key_offsets = absolute_output_offsets_to_relative(input_token_to_key.key_offsets);
        tx.vin.push_back(input_token_to_key);
      }
      else if (src_entr.referenced_output_type == tx_out_type::out_cash)
      {
        //put key image into tx input
        txin_to_key input_to_key = AUTO_VAL_INIT(input_to_key);
        input_to_key.amount = src_entr.amount;
        input_to_key.k_image = img;

        //fill outputs array and use relative offsets
        for (const tx_source_entry::output_entry &out_entry: src_entr.outputs)
          input_to_key.key_offsets.push_back(out_entry.first);

        input_to_key.key_offsets = absolute_output_offsets_to_relative(input_to_key.key_offsets);
        tx.vin.push_back(input_to_key);
      }
      else
      {
        LOG_ERROR("Unsuported input!!");
        return false;
      }


    }

    if (shuffle_outs)
    {
      std::shuffle(destinations.begin(), destinations.end(), std::default_random_engine(crypto::rand<unsigned int>()));
    }

    // sort ins by their key image
    std::vector<size_t> ins_order(sources.size());
    for (size_t n = 0; n < sources.size(); ++n)
      ins_order[n] = n;
    std::sort(ins_order.begin(), ins_order.end(), [&](const size_t i0, const size_t i1) {
      const crypto::key_image &tk0_key_image = *boost::apply_visitor(key_image_visitor(), tx.vin[i0]);
      const crypto::key_image &tk1_key_image = *boost::apply_visitor(key_image_visitor(), tx.vin[i1]);
      return memcmp(&tk0_key_image, &tk1_key_image, sizeof(tk1_key_image)) > 0;
    });
    tools::apply_permutation(ins_order, [&] (size_t i0, size_t i1) {
      std::swap(tx.vin[i0], tx.vin[i1]);
      std::swap(in_contexts[i0], in_contexts[i1]);
      std::swap(sources[i0], sources[i1]);
    });

    // figure out if we need to make additional tx pubkeys
    size_t num_stdaddresses = 0;
    size_t num_subaddresses = 0;
    account_public_address single_dest_subaddress = AUTO_VAL_INIT(single_dest_subaddress);
    classify_addresses(destinations, change_addr, num_stdaddresses, num_subaddresses, single_dest_subaddress);

    // if this is a single-destination transfer to a subaddress, we set the tx pubkey to R=s*D
    if (num_stdaddresses == 0 && num_subaddresses == 1)
    {
      txkey_pub = rct::rct2pk(hwdev.scalarmultKey(rct::pk2rct(single_dest_subaddress.m_spend_public_key), rct::sk2rct(tx_key)));
    }
    else
    {
      txkey_pub = rct::rct2pk(hwdev.scalarmultBase(rct::sk2rct(tx_key)));
    }
    remove_field_from_tx_extra(tx.extra, typeid(tx_extra_pub_key));
    add_tx_pub_key_to_extra(tx, txkey_pub);

    std::vector<crypto::public_key> additional_tx_public_keys;

    // we don't need to include additional tx keys if:
    //   - all the destinations are standard addresses
    //   - there's only one destination which is a subaddress
    bool need_additional_txkeys = num_subaddresses > 0 && (num_stdaddresses > 0 || num_subaddresses > 1);
    if (need_additional_txkeys)
      CHECK_AND_ASSERT_MES(destinations.size() == additional_tx_keys.size(), false, "Wrong amount of additional tx keys");

    uint64_t summary_outs_money = 0;
    uint64_t summary_outs_tokens = 0;
    //fill outputs
    size_t output_index = 0;
    for(const tx_destination_entry& dst_entr: destinations)
    {
      CHECK_AND_ASSERT_MES(dst_entr.amount > 0 || dst_entr.token_amount > 0 || tx.version > 1, false, "Destination with wrong amount: " << dst_entr.amount << " or token amount " << dst_entr.token_amount);
      crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
      crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);

      // make additional tx pubkey if necessary
      keypair additional_txkey = AUTO_VAL_INIT(additional_txkey);
      if (need_additional_txkeys)
      {
        additional_txkey.sec = additional_tx_keys[output_index];
        if (dst_entr.is_subaddress)
          additional_txkey.pub = rct::rct2pk(hwdev.scalarmultKey(rct::pk2rct(dst_entr.addr.m_spend_public_key), rct::sk2rct(additional_txkey.sec)));
        else
          additional_txkey.pub = rct::rct2pk(hwdev.scalarmultBase(rct::sk2rct(additional_txkey.sec)));
      }

      bool r;
      if (change_addr && dst_entr.addr == *change_addr)
      {
        // sending change to yourself; derivation = a*R
        r = hwdev.generate_key_derivation(txkey_pub, sender_account_keys.m_view_secret_key, derivation);
        CHECK_AND_ASSERT_MES(r, false, "at creation outs: failed to generate_key_derivation(" << txkey_pub << ", " << sender_account_keys.m_view_secret_key << ")");
      }
      else
      {
        // sending to the recipient; derivation = r*A (or s*C in the subaddress scheme)
        r = hwdev.generate_key_derivation(dst_entr.addr.m_view_public_key, dst_entr.is_subaddress && need_additional_txkeys ? additional_txkey.sec : tx_key, derivation);
        CHECK_AND_ASSERT_MES(r, false, "at creation outs: failed to generate_key_derivation(" << dst_entr.addr.m_view_public_key << ", " << (dst_entr.is_subaddress && need_additional_txkeys ? additional_txkey.sec : tx_key) << ")");
      }

      if (need_additional_txkeys)
      {
        additional_tx_public_keys.push_back(additional_txkey.pub);
      }

      r = hwdev.derive_public_key(derivation, output_index, dst_entr.addr.m_spend_public_key, out_eph_public_key);
      CHECK_AND_ASSERT_MES(r, false, "at creation outs: failed to derive_public_key(" << derivation << ", " << output_index << ", "<< dst_entr.addr.m_spend_public_key << ")");

      hwdev.add_output_key_mapping(dst_entr.addr.m_view_public_key, dst_entr.addr.m_spend_public_key, dst_entr.is_subaddress, output_index, amount_keys.back(), out_eph_public_key);

      tx_out out = AUTO_VAL_INIT(out);

      if (dst_entr.token_transaction) {
        out.token_amount = dst_entr.token_amount;
        out.amount = 0;
        txout_token_to_key ttk = AUTO_VAL_INIT(ttk);
        ttk.key = out_eph_public_key;
        out.target = ttk;
        tx.vout.push_back(out);
      } else {
        out.amount = dst_entr.amount;
        out.token_amount = 0;
        txout_to_key tk = AUTO_VAL_INIT(tk);
        tk.key = out_eph_public_key;
        out.target = tk;
        tx.vout.push_back(out);
      }

      output_index++;
      summary_outs_money += dst_entr.amount;
      summary_outs_tokens += dst_entr.token_amount;
    }
    CHECK_AND_ASSERT_MES(additional_tx_public_keys.size() == additional_tx_keys.size(), false, "Internal error creating additional public keys");

    remove_field_from_tx_extra(tx.extra, typeid(tx_extra_additional_pub_keys));

    LOG_PRINT_L2("tx pubkey: " << txkey_pub);
    if (need_additional_txkeys)
    {
      LOG_PRINT_L2("additional tx pubkeys: ");
      for (size_t i = 0; i < additional_tx_public_keys.size(); ++i)
        LOG_PRINT_L2(additional_tx_public_keys[i]);
      add_additional_tx_pub_keys_to_extra(tx.extra, additional_tx_public_keys);
    }

    //check money
    if(summary_outs_money > summary_inputs_money )
    {
      LOG_ERROR("Transaction inputs money ("<< summary_inputs_money << ") less than outputs money (" << summary_outs_money << ")");
      return false;
    }

    //check tokens
    if(summary_outs_tokens > summary_inputs_tokens )
    {
      LOG_ERROR("Transaction inputs tokens ("<< summary_inputs_tokens << ") less than outputs tokens (" << summary_outs_tokens << ")");
      return false;
    }

    // check for watch only wallet
    bool zero_secret_key = true;
    for (size_t i = 0; i < sizeof(sender_account_keys.m_spend_secret_key); ++i)
      zero_secret_key &= (sender_account_keys.m_spend_secret_key.data[i] == 0);
    if (zero_secret_key)
    {
      MDEBUG("Null secret key, skipping signatures");
    }

    if (tx.version == 1)
    {
      //generate ring signatures
      crypto::hash tx_prefix_hash = AUTO_VAL_INIT(tx_prefix_hash);
      get_transaction_prefix_hash(tx, tx_prefix_hash);

      std::stringstream ss_ring_s;
      size_t i = 0;
      for(const tx_source_entry& src_entr:  sources)
      {
        ss_ring_s << "pub_keys:" << ENDL;
        std::vector<const crypto::public_key*> keys_ptrs;
        std::vector<crypto::public_key> keys(src_entr.outputs.size());
        size_t ii = 0;

        for(const tx_source_entry::output_entry& o: src_entr.outputs)
        {
          keys[ii] = rct2pk(o.second.dest);
          keys_ptrs.push_back(&keys[ii]);
          ss_ring_s << o.second.dest << ENDL;
          ++ii;
        }
        tx.signatures.push_back(std::vector<crypto::signature>());
        std::vector<crypto::signature>& sigs = tx.signatures.back();
        sigs.resize(src_entr.outputs.size());
        if (!zero_secret_key) {
          const crypto::key_image &k_image = *boost::apply_visitor(key_image_visitor(), tx.vin[i]);
          if (src_entr.referenced_output_type == tx_out_type::out_bitcoin_migration) {
            public_key spend_public_key = AUTO_VAL_INIT(spend_public_key);
            CHECK_AND_ASSERT_MES(crypto::secret_key_to_public_key(sender_account_keys.m_spend_secret_key, spend_public_key), false, "Could not create public_key from private_key");
            crypto::generate_signature(tx_prefix_hash, spend_public_key, sender_account_keys.m_spend_secret_key, sigs[0]);
          } else {
            crypto::generate_ring_signature(tx_prefix_hash, k_image, keys_ptrs, in_contexts[i].in_ephemeral.sec, src_entr.real_output, sigs.data());
          }
        }
        ss_ring_s << "signatures:" << ENDL;
        std::for_each(sigs.begin(), sigs.end(), [&](const crypto::signature& s){ss_ring_s << s << ENDL;});
        ss_ring_s << "prefix_hash:" << tx_prefix_hash << ENDL << "in_ephemeral_key: " << in_contexts[i].in_ephemeral.sec << ENDL << "real_output: " << src_entr.real_output << ENDL;
        i++;
      }

      MCINFO("construct_tx", "transaction_created: " << get_transaction_hash(tx) << ENDL << obj_to_json_str(tx) << ENDL << ss_ring_s.str());
    }
    else
    {
      LOG_ERROR("Transaction version>=2 not supported");
      return false;

    }

    tx.invalidate_hashes();

    return true;
  }

  //---------------------------------------------------------------
  bool construct_tx_and_get_tx_key(const account_keys& sender_account_keys, const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses, std::vector<tx_source_entry>& sources,
          std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::account_public_address>& change_addr, std::vector<uint8_t> extra,
          transaction& tx, uint64_t unlock_time, crypto::secret_key &tx_key, std::vector<crypto::secret_key> &additional_tx_keys)
  {
    hw::device &hwdev = sender_account_keys.get_device();
    hwdev.open_tx(tx_key);

    // figure out if we need to make additional tx pubkeys
    size_t num_stdaddresses = 0;
    size_t num_subaddresses = 0;
    account_public_address single_dest_subaddress = AUTO_VAL_INIT(single_dest_subaddress);
    classify_addresses(destinations, change_addr, num_stdaddresses, num_subaddresses, single_dest_subaddress);
    bool need_additional_txkeys = num_subaddresses > 0 && (num_stdaddresses > 0 || num_subaddresses > 1);
    if (need_additional_txkeys)
    {
      additional_tx_keys.clear();
      for (const auto &d: destinations)
        additional_tx_keys.push_back(keypair::generate(sender_account_keys.get_device()).sec);
    }

    //check if we are dealing with advanced transaction
    bool advanced_transaction = std::any_of(destinations.begin(), destinations.end(), [](const tx_destination_entry &de) {return de.script_output;});

    bool r;
    if (advanced_transaction)
      r = construct_advanced_tx_with_tx_key(sender_account_keys, subaddresses, sources, destinations, change_addr, extra, tx, unlock_time, tx_key, additional_tx_keys);
    else
      r = construct_tx_with_tx_key(sender_account_keys, subaddresses, sources, destinations, change_addr, extra, tx, unlock_time, tx_key, additional_tx_keys);

    hwdev.close_tx();
    return r;
  }
  //---------------------------------------------------------------
  bool construct_tx(const account_keys& sender_account_keys, std::vector<tx_source_entry>& sources, const std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::account_public_address>& change_addr, std::vector<uint8_t> extra, transaction& tx, uint64_t unlock_time)
  {
     std::unordered_map<crypto::public_key, cryptonote::subaddress_index> subaddresses;
     subaddresses[sender_account_keys.m_account_address.m_spend_public_key] = {0,0};
     crypto::secret_key tx_key;
     std::vector<crypto::secret_key> additional_tx_keys;
     std::vector<tx_destination_entry> destinations_copy = destinations;
     return construct_tx_and_get_tx_key(sender_account_keys, subaddresses, sources, destinations_copy, change_addr, extra, tx, unlock_time, tx_key, additional_tx_keys);
  }
  //---------------------------------------------------------------
  bool generate_genesis_block(
      block& bl
    , std::string const & genesis_tx
    , uint32_t nonce
    )
  {
    //genesis block
    bl = boost::value_initialized<block>();

    blobdata tx_bl;
    bool r = string_tools::parse_hexstr_to_binbuff(genesis_tx, tx_bl);
    CHECK_AND_ASSERT_MES(r, false, "failed to parse coinbase tx from hard coded blob");
    r = parse_and_validate_tx_from_blob(tx_bl, bl.miner_tx);
    CHECK_AND_ASSERT_MES(r, false, "failed to parse coinbase tx from hard coded blob");
    bl.major_version = CURRENT_BLOCK_MAJOR_VERSION;
    bl.minor_version = CURRENT_BLOCK_MINOR_VERSION;
    bl.timestamp = 0;
    bl.nonce = nonce;
    miner::find_nonce_for_given_block(bl, 1, 0);
    bl.invalidate_hashes();
    return true;
  }
  //---------------------------------------------------------------
  cryptonote::tx_source_entry::output_entry generate_migration_bitcoin_transaction_output(const account_keys& sender_account_keys, const crypto::hash bitcoin_tx_hash, uint64_t token_amount)
  {
    cryptonote::tx_source_entry::output_entry bitcoin_output{0u, rct::ctkey(
        {rct::pk2rct(sender_account_keys.m_account_address.m_spend_public_key),
         rct::zeroCommit(token_amount)})};
    return bitcoin_output;
  }

  bool generate_migration_key_image(const crypto::hash &bitcoin_transaction_hash, crypto::key_image &key_image)
  {
    // todo igor for now just place the transaction hash into key_image it should be enough
    CHECK_AND_ASSERT_MES(sizeof(key_image.data) == sizeof(bitcoin_transaction_hash.data), false, "key_image and bitcoin_hash do not have the same size.");
    memcpy(key_image.data, bitcoin_transaction_hash.data, sizeof(key_image.data));
    return true;
  }

  namespace fakechain //for token core tests. Not nice, but only possible without much refactoring 
  {
    static crypto::public_key MIGRATION_FAKECHAIN_VALIDATION_PUBLIC_KEY;

    void set_core_tests_public_key(const crypto::public_key& publicKey)
    {
      MIGRATION_FAKECHAIN_VALIDATION_PUBLIC_KEY = publicKey;
    }

    const crypto::public_key& get_core_tests_public_key()
    {
      return MIGRATION_FAKECHAIN_VALIDATION_PUBLIC_KEY;
    }
  }

  const std::string get_genesis_tx_as_str(cryptonote::network_type nettype)
  {
    switch (nettype)
    {
      case cryptonote::network_type::MAINNET: return config::GENESIS_TX;break;
      case cryptonote::network_type::STAGENET: return config::stagenet::GENESIS_TX;break;
      case cryptonote::network_type::TESTNET: return config::testnet::GENESIS_TX;break;
      default:
        return "";
    }
  }

  bool extract_migration_pubkey_from_genesis_transaction(cryptonote::network_type nettype, crypto::public_key& migration_key)
  {
    //genesis block
    cryptonote::transaction genesis_tx;
    std::string const & genesis_tx_str = get_genesis_tx_as_str(nettype);
    blobdata tx_bl;

    bool r = string_tools::parse_hexstr_to_binbuff(genesis_tx_str, tx_bl);
    CHECK_AND_ASSERT_MES(r, false, "failed to parse genesis tx from hard coded blob");

    r = parse_and_validate_tx_from_blob(tx_bl, genesis_tx);
    CHECK_AND_ASSERT_MES(r, false, "failed to parse genesis tx from hard coded blob");

    int migration_key_index = 0;
    switch (nettype)
    {
      case cryptonote::network_type::MAINNET: migration_key_index = config::MIGRATION_GENESIS_PUBKEY_INDEX; break;
      case cryptonote::network_type::STAGENET: migration_key_index = config::stagenet::MIGRATION_GENESIS_PUBKEY_INDEX; break;
      case cryptonote::network_type::TESTNET: migration_key_index = config::testnet::MIGRATION_GENESIS_PUBKEY_INDEX; break;
      default: migration_key_index = 0; break;
    }
    migration_key = get_migration_pub_key_from_extra(genesis_tx.extra, migration_key_index);
    return true;
  }


  bool get_migration_verification_public_key(cryptonote::network_type nettype, crypto::public_key &public_key)
  {
    switch (nettype) {
      case network_type::TESTNET:
      case network_type::STAGENET:
      case network_type::MAINNET:
        return extract_migration_pubkey_from_genesis_transaction(nettype, public_key);
        break;
      case network_type::FAKECHAIN:
        public_key = cryptonote::fakechain::get_core_tests_public_key();
        break;
      default:
        LOG_ERROR("Invalid network type");
        return false;
    }
    return true;
  }
  //---------------------------------------------------------------
}
