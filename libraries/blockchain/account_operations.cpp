#include <bts/blockchain/account_operations.hpp>
#include <bts/blockchain/chain_interface.hpp>
#include <bts/blockchain/exceptions.hpp>
#include <bts/blockchain/transaction_evaluation_state.hpp>
#include <fc/time.hpp>

namespace bts { namespace blockchain {

   static const fc::microseconds one_year = fc::seconds( 60*60*24*365 );

   string get_parent_account_name( const string& child )
   {
      auto pos = child.find( '.' );
      if( pos == string::npos ) return string();
      return child.substr( pos + 1, string::npos );
   }

   bool register_account_operation::is_delegate()const
   {
       return delegate_pay_rate <= 100;
   }

   void register_account_operation::evaluate( transaction_evaluation_state& eval_state )
   { try {
      if( !eval_state._current_state->is_valid_account_name( this->name ) )
         FC_CAPTURE_AND_THROW( invalid_account_name, (name) );

      oaccount_record current_account = eval_state._current_state->get_account_record( this->name );
      if( current_account.valid() )
          FC_CAPTURE_AND_THROW( account_already_registered, (name) );

      current_account = eval_state._current_state->get_account_record( this->owner_key );
      if( current_account.valid() )
          FC_CAPTURE_AND_THROW( account_key_in_use, (this->owner_key)(current_account) );

      current_account = eval_state._current_state->get_account_record( this->active_key );
      if( current_account.valid() )
          FC_CAPTURE_AND_THROW( account_key_in_use, (this->active_key)(current_account) );

      string parent_name = get_parent_account_name( this->name );
      if( !parent_name.empty() )
      {
         const oaccount_record parent_record = eval_state._current_state->get_account_record( parent_name );
         if( !parent_record.valid() )
             FC_CAPTURE_AND_THROW( unknown_account_name, (parent_name) );

         if( parent_record->is_retracted() )
            FC_CAPTURE_AND_THROW( parent_account_retracted, (parent_record) );

         if( !eval_state.check_signature( parent_record->active_key() ) )
            FC_CAPTURE_AND_THROW( missing_parent_account_signature, (parent_record) );
      }

      const time_point_sec now = eval_state._current_state->now();

      account_record new_record;
      new_record.id                = eval_state._current_state->new_account_id();
      new_record.name              = this->name;
      new_record.public_data       = this->public_data;
      new_record.owner_key         = this->owner_key;
      new_record.set_active_key( now, this->active_key );
      new_record.registration_date = now;
      new_record.last_update       = now;
      new_record.meta_data         = this->meta_data;

      if( this->is_delegate() )
      {
          new_record.delegate_info = delegate_stats();
          new_record.delegate_info->pay_rate = this->delegate_pay_rate;
          new_record.delegate_info->block_signing_key = this->active_key;

          const asset reg_fee( eval_state._current_state->get_delegate_registration_fee( this->delegate_pay_rate ), 0 );
          eval_state.required_fees += reg_fee;
      }

      eval_state._current_state->store_account_record( new_record );
   } FC_CAPTURE_AND_RETHROW( (*this) ) }

   bool update_account_operation::is_delegate()const
   {
       return delegate_pay_rate <= 100;
   }

   void update_account_operation::evaluate( transaction_evaluation_state& eval_state )
   { try {
      auto current_record = eval_state._current_state->get_account_record( this->account_id );
      if( !current_record ) FC_CAPTURE_AND_THROW( unknown_account_id, (account_id) );
      if( current_record->is_retracted() ) FC_CAPTURE_AND_THROW( account_retracted, (current_record) );

      string parent_name = get_parent_account_name( current_record->name );
      if( this->active_key && *this->active_key != current_record->active_key() )
      {
         // check signature of any parent record...
         if( parent_name.size() == 0 )
         {
            if( !eval_state.check_signature(current_record->owner_key)  )
               FC_CAPTURE_AND_THROW( missing_signature, (current_record->owner_key) );
         }
         else
         {
             auto parent_record = eval_state._current_state->get_account_record( parent_name );
             if( !parent_record )
                FC_CAPTURE_AND_THROW( unknown_parent_account_name, (parent_name) );

             bool verified = false;
             while( !verified && parent_record.valid() )
             {
                if( parent_record->is_retracted() )
                   FC_CAPTURE_AND_THROW( parent_account_retracted, (parent_name) );

                if( eval_state.check_signature( parent_record->owner_key ) ||
                    eval_state.check_signature( parent_record->active_address() ) )
                   verified = true;
                else
                {
                   parent_name = get_parent_account_name( parent_name );
                   parent_record = eval_state._current_state->get_account_record( parent_name );
                }
             }
             if( !verified )
             {
                string message = "updating the active key for this account requires the signature "
                                 "of the owner or one of their parent accounts";
                FC_CAPTURE_AND_THROW( missing_signature, (message) );
             }
         }
      }
      else
      {
         bool verified = eval_state.check_signature( current_record->active_address() );
         if( !verified ) verified = eval_state.check_signature( current_record->owner_key );

         if( !verified && parent_name.size() )
         {
             auto parent_record = eval_state._current_state->get_account_record( parent_name );
             while( !verified && parent_record.valid() )
             {
                if( parent_record->is_retracted() )
                   FC_CAPTURE_AND_THROW( parent_account_retracted, (parent_name) );

                if( eval_state.check_signature( parent_record->owner_key ) ||
                    eval_state.check_signature( parent_record->active_address() ) )
                   verified = true;
                else
                {
                   parent_name = get_parent_account_name( parent_name );
                   parent_record = eval_state._current_state->get_account_record( parent_name );
                }
             }
         }
         if( !verified )
         {
            string message = "updating the active key for this account requires the signature "
                             "of the owner or one of their parent accounts";
            FC_CAPTURE_AND_THROW( missing_signature, (message) );
         }
      }

      if( this->active_key.valid() )
      {
         // Update active key
         current_record->set_active_key( eval_state._current_state->now(), *this->active_key );
         auto account_with_same_key = eval_state._current_state->get_account_record( address(*this->active_key) );
         if( account_with_same_key )
            FC_CAPTURE_AND_THROW( account_key_in_use, (active_key)(account_with_same_key) );

#ifndef WIN32
#warning Until block signing keys can be changed, they must always be equal to the active key
#endif
         if( current_record->is_delegate() )
            current_record->delegate_info->block_signing_key = current_record->active_key();
      }

      if( this->public_data.valid() )
      {
         current_record->public_data  = *this->public_data;
      }

      if( current_record->is_delegate() )
      {
          // Delegates accounts cannot revert to a normal account
          FC_ASSERT( this->is_delegate() );
      }

      if( this->is_delegate() )
      {
         if( current_record->is_delegate() )
         {
            // Delegates cannot increase their pay rate
            FC_ASSERT( current_record->delegate_info->pay_rate >= this->delegate_pay_rate );
            current_record->delegate_info->pay_rate = this->delegate_pay_rate;
         }
         else
         {
            current_record->delegate_info = delegate_stats();
            current_record->delegate_info->pay_rate = this->delegate_pay_rate;
            current_record->delegate_info->block_signing_key = current_record->active_key();
            const asset reg_fee( eval_state._current_state->get_delegate_registration_fee( this->delegate_pay_rate ), 0 );
            eval_state.required_fees += reg_fee;
         }
      }

      current_record->last_update = eval_state._current_state->now();

      eval_state._current_state->store_account_record( *current_record );
   } FC_CAPTURE_AND_RETHROW( (*this) ) }

   void withdraw_pay_operation::evaluate( transaction_evaluation_state& eval_state )
   { try {
      if( this->amount <= 0 )
          FC_CAPTURE_AND_THROW( negative_withdraw, (amount) );

      auto account_id = abs( this->account_id );
      auto account = eval_state._current_state->get_account_record( account_id );
      if( !account )
          FC_CAPTURE_AND_THROW( unknown_account_id, (account_id) );

      if( account->is_retracted() )
          FC_CAPTURE_AND_THROW( account_retracted, (account) );

      if( !account->is_delegate() )
          FC_CAPTURE_AND_THROW( not_a_delegate, (account) );

      auto active_key = account->active_key();
      if( !eval_state.check_signature( active_key ) )
         FC_CAPTURE_AND_THROW( missing_signature, (active_key) );

      eval_state.net_delegate_votes[ account_id ].votes_for -= this->amount;

      if( account->delegate_info->pay_balance < this->amount )
         FC_CAPTURE_AND_THROW( insufficient_funds, (account)(amount) );

      account->delegate_info->pay_balance -= this->amount;

      eval_state._current_state->store_account_record( *account );
      eval_state.add_balance( asset(this->amount, 0) );

   } FC_CAPTURE_AND_RETHROW( (*this) ) }

   void link_account_operation::evaluate( transaction_evaluation_state& eval_state )
   { try {
      FC_ASSERT( !"Link account operation is not enabled yet!" );

      auto source_account_rec = eval_state._current_state->get_account_record( source_account );
      FC_ASSERT( source_account_rec.valid() );

      auto destination_account_rec = eval_state._current_state->get_account_record( destination_account );
      FC_ASSERT( destination_account_rec.valid() );

      if( !eval_state.check_signature( source_account_rec->active_key() ) )
      {
         FC_CAPTURE_AND_THROW( missing_signature, (source_account_rec->active_key()) );
      }

      // STORE LINK...
   } FC_CAPTURE_AND_RETHROW( (eval_state) ) }

   void update_block_signing_key::evaluate( transaction_evaluation_state& eval_state )
   {
      FC_ASSERT( !"Update block signing key operation is not enabled yet!" );

      auto account_rec = eval_state._current_state->get_account_record( this->account_id );
      FC_ASSERT( account_rec.valid() );
      FC_ASSERT( account_rec->is_delegate() );
      if( eval_state.check_signature( account_rec->active_key() ) ||
          eval_state.check_signature( account_rec->owner_key ) ||
          eval_state.check_signature( account_rec->delegate_info->block_signing_key )  )
      {
          account_rec->delegate_info->block_signing_key = this->block_signing_key;
      }
      else
      {
         FC_CAPTURE_AND_THROW( missing_signature, (account_rec->owner_key) );
      }

   }

} } // bts::blockchain
