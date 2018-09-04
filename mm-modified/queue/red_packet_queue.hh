/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef RED_PACKET_QUEUE_HH
#define RED_PACKET_QUEUE_HH

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <queue>
#include <string>

#include "queued_packet.hh"
#include "abstract_packet_queue.hh"
#include "exception.hh"
#include "ezio.hh"

class RedPacketQueue : public AbstractPacketQueue
{
private:
    std::queue<QueuedPacket> internal_queue_ {};

    int queue_size_in_bytes_ = 0;
    int min_queue_size_threshold_in_bytes_ = 0;
    int max_queue_size_threshold_in_bytes_ = 0;
    double average_queue_size_in_bytes_ = 0;
    const double W = 0.002;
    const double packet_rate_ = 800;
    double drop_probability_ = 0;
    int count_ = 0;
    long q_time_ = 0;

    unsigned int get_arg( const std::string & args, const std::string & name )
    {
        auto offset = args.find( name );
        if ( offset == std::string::npos ) {
            std::cout << "[args]: " << args << std::endl;
            std::cout << "[get_arg] Could not find " << name << std::endl;
            return 0; /* default value */
        } else {
            /* extract the value */

            /* advance by length of name */
            offset += name.size();

            /* make sure next char is "=" */
            if ( args.substr( offset, 1 ) != "=" ) {
                throw std::runtime_error( "could not parse queue arguments: " + args );
            }

            /* advance by length of "=" */
            offset++;

            /* find the first non-digit character */
            auto offset2 = args.substr( offset ).find_first_not_of( "0123456789" );

            auto digit_string = args.substr( offset ).substr( 0, offset2 );

            if ( digit_string.empty() ) {
                throw std::runtime_error( "could not parse queue arguments: " + args );
            }

            return myatoi( digit_string );
        }
    }

public:
    RedPacketQueue( const std::string & args )
        : min_queue_size_threshold_in_bytes_( get_arg( args, "min_bytes") ),
          max_queue_size_threshold_in_bytes_( get_arg( args, "max_bytes") ),
          drop_probability_( get_arg( args, "drop_percentage" ) / 100.0)
    {
        std::cout << "Min queue: " << min_queue_size_threshold_in_bytes_
                  << ", Max queue: " << max_queue_size_threshold_in_bytes_
                  << ", Drop probability: " << drop_probability_
                  << std::endl;
        if (drop_probability_ < 0 || drop_probability_ > 1) {
            throw std::runtime_error( "Invalid RedPacketQueue drop percentage" );
        }
    }

    void enqueue( QueuedPacket && p ) override
    {
        if (queue_size_in_bytes_ > 0) {
            average_queue_size_in_bytes_ =
                (1 - W) * average_queue_size_in_bytes_ + W * queue_size_in_bytes_;
        } else {
            unsigned long curr_time = 
                std::chrono::system_clock::now().time_since_epoch() /
                std::chrono::milliseconds(1);
            double m = packet_rate_ * (curr_time - q_time_);
            average_queue_size_in_bytes_ = pow(1 - W, m) * average_queue_size_in_bytes_;
        }

        if (average_queue_size_in_bytes_ >= max_queue_size_threshold_in_bytes_) {
            count_ = 0;
            return;
        } else if (average_queue_size_in_bytes_ >= min_queue_size_threshold_in_bytes_) {
            ++count_;
            
            double p_b = drop_probability_ *
                (average_queue_size_in_bytes_ - min_queue_size_threshold_in_bytes_) /
                (max_queue_size_threshold_in_bytes_ - min_queue_size_threshold_in_bytes_);
            // double p_a = p_b / (1 - count_ * p_b);
            p_b = p_b * p.contents.size() / 1500.0;
            double p_a = p_b / (1 - count_ * p_b);

            float r = static_cast <float> (rand()) / static_cast <float> (RAND_MAX);
            if (r < p_a) {
                count_ = 0;
                return;
            }
        }

        count_ = -1;
        
        queue_size_in_bytes_ += p.contents.size();
        internal_queue_.emplace( std::move( p ) );
    }

    QueuedPacket dequeue( void ) override
    {
        assert( not internal_queue_.empty() );

        QueuedPacket ret = std::move( internal_queue_.front() );
        internal_queue_.pop();

        queue_size_in_bytes_ -= ret.contents.size();

        if (queue_size_in_bytes_ == 0) {
            q_time_ =
                std::chrono::system_clock::now().time_since_epoch() /
                std::chrono::milliseconds(1);
        }

        return ret;
    }

    bool empty( void ) const override
    {
        return internal_queue_.empty();
    }

    std::string to_string( void ) const override
    {
        return "red";
    }
};

#endif /* RED_PACKET_QUEUE_HH */ 
