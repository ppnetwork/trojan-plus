/*
 * This file is part of the Trojan Plus project.
 * Trojan is an unidentifiable mechanism that helps you bypass GFW.
 * Trojan Plus is derived from original trojan project and writing 
 * for more experimental features.
 * Copyright (C) 2020 The Trojan Plus Group Authors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tunsession.h"

#include <ostream>
#include <string>
#include <lwipopts.h>

#include "core/service.h"
#include "core/utils.h"
#include "proto/trojanrequest.h"
#include "proto/udppacket.h"

using namespace std;
using namespace boost::asio::ip;

static const char* const localhost_ip_addr = "127.0.0.1";

TUNSession::TUNSession(Service* _service, bool _is_udp) : 
    Session(_service, _service->get_config()),
    m_service(_service),
    m_recv_buf_ack_length(0),
    m_out_socket(_service->get_io_context(), _service->get_ssl_context()),
    m_out_resolver(_service->get_io_context()),
    m_destroyed(false),
    m_close_from_tundev_flag(false),
    m_connected(false),
    m_udp_timout_timer(_service->get_io_context()){

    set_udp_forward_session(_is_udp);
    get_pipeline_component().allocate_session_id();

    m_sending_data_cache.set_is_connected_func([this](){ return !is_destroyed() && m_connected; });
    m_sending_data_cache.set_async_writer([this](const boost::asio::streambuf& data, SentHandler&& handler) {
        auto self = shared_from_this();
        boost::asio::async_write(m_out_socket, data.data(), 
          [this, self, handler](const boost::system::error_code error, size_t length) {

            reset_udp_timeout();
            if (error) {
                output_debug_info_ec(error);
                destroy();
            }

            if(get_sent_len() == 0){
                output_debug_info();
                _log_with_date_time(to_string(m_local_addr.port()) + " session_id: " + to_string(get_session_id()) + 
                    " inc_sent_len from 0 to size: " + to_string(length), Log::INFO);
            }
            inc_sent_len(length);

            handler(error);
        });
    });
}

TUNSession::~TUNSession(){
    get_pipeline_component().free_session_id();
}

udp::endpoint TUNSession::get_redirect_local_remote_addr(bool output_log /*= false*/) const {
    auto remote_addr = m_remote_addr_udp;
    remote_addr.address(make_address_v4(localhost_ip_addr));
    if(output_log){
        _log_with_date_time(m_remote_addr_udp.address().to_string() + " redirect to local for test");
    }

    return remote_addr;
}

void TUNSession::start(){
    reset_udp_timeout();
    set_start_time(time(nullptr));
    auto self = shared_from_this();
    auto cb = [this, self](){
        m_connected  = true;

        if(!m_service->is_use_pipeline()){
            boost::system::error_code ec;
            auto endpoint = m_out_socket.next_layer().local_endpoint(ec);
            _log_with_endpoint(endpoint, "TUNSession session_id: " + to_string(get_session_id()) + " started", Log::INFO);
        }else{
            if(is_udp_forward_session()){
                _log_with_endpoint(m_local_addr_udp, "TUNSession session_id: " + to_string(get_session_id()) + " started in pipeline", Log::INFO);
            }else{
                _log_with_endpoint(m_local_addr, "TUNSession session_id: " + to_string(get_session_id()) + " started in pipeline", Log::INFO);
            }
        }

        auto insert_pwd = [this](){
            if(is_udp_forward_session()){
                streambuf_append(m_send_buf, TrojanRequest::generate(get_config().get_password().cbegin()->first, 
                    get_config().get_tun().redirect_local ? localhost_ip_addr : m_remote_addr_udp.address().to_string().c_str(),
                    m_remote_addr_udp.port(), false));
            }else{
                auto remote_addr = m_remote_addr.address().to_string();
                if(get_config().get_tun().redirect_local){
                    _log_with_date_time(remote_addr + " redirect to local for test");
                    remote_addr = localhost_ip_addr;
                }
                streambuf_append(m_send_buf, TrojanRequest::generate(get_config().get_password().cbegin()->first, 
                    remote_addr, m_remote_addr.port(), true));
            }
        };

        if(m_send_buf.size() > 0){
            boost::asio::streambuf tmp_buf;
            streambuf_append(tmp_buf, m_send_buf);
            m_send_buf.consume(m_send_buf.size());
            insert_pwd();
            streambuf_append(m_send_buf, tmp_buf);
        }else{
            insert_pwd();
        }
        
        _log_with_date_time(to_string(m_local_addr.port()) + " session_id: " + to_string(get_session_id()) + 
                " increase m_sending_len from "+to_string(m_sending_len)+" size: " + to_string(m_send_buf.size()), Log::INFO);
        m_sending_len += m_send_buf.size();

        out_async_send_impl(streambuf_to_string_view(m_send_buf), [this](boost::system::error_code ec){
            if(ec){
                output_debug_info_ec(ec);
                destroy();
                return;
            }
            if(!m_wait_connected_handler.empty()){
                for(auto& h : m_wait_connected_handler){
                    h(boost::system::error_code());
                }
                m_wait_connected_handler.clear();
            }
            out_async_read();
        });
        m_send_buf.consume(m_send_buf.size());
    };

    if(m_service->is_use_pipeline()){
        cb();
    }else{
        m_service->get_config().prepare_ssl_reuse(m_out_socket);
        if(is_udp_forward_session()){
            connect_remote_server_ssl(this, m_service->get_config().get_remote_addr(), to_string(m_service->get_config().get_remote_port()), 
                m_out_resolver, m_out_socket, m_local_addr_udp ,  cb);
        }else{
            connect_remote_server_ssl(this, m_service->get_config().get_remote_addr(), to_string(m_service->get_config().get_remote_port()), 
                m_out_resolver, m_out_socket, m_local_addr,  cb);
        }
        
    }
}

void TUNSession::reset_udp_timeout(){
    if(is_udp_forward_session()){
        m_udp_timout_timer.cancel();

        m_udp_timout_timer.expires_after(chrono::seconds(m_service->get_config().get_udp_timeout()));
        auto self = shared_from_this();
        m_udp_timout_timer.async_wait([this, self](const boost::system::error_code error) {
            if (!error) {
                _log_with_endpoint(m_local_addr_udp, "session_id: " + to_string(get_session_id()) + " UDP TUNSession timeout", Log::INFO);
                destroy();
            }
        });
    }
}

void TUNSession::destroy(bool pipeline_call){
    if(m_destroyed){
        return;
    }
    m_destroyed = true;

    auto note_str = "TUNSession session_id: " + to_string(get_session_id()) + " disconnected, " + 
        to_string(get_recv_len()) + " bytes received, " + to_string(get_sent_len()) + " bytes sent, lasted for " + 
        to_string(time(nullptr) - get_start_time()) + " seconds";

    if(is_udp_forward_session()){
        _log_with_endpoint(m_local_addr_udp, note_str, Log::INFO);
    }else{
        _log_with_endpoint(m_local_addr, note_str, Log::INFO);
    }    

    m_wait_ack_handler.clear();
    m_out_resolver.cancel();   
    m_udp_timout_timer.cancel();
    shutdown_ssl_socket(this, m_out_socket);

    if(!pipeline_call && m_service->is_use_pipeline()){
        get_service()->session_destroy_in_pipeline(*this);
    }

    if(!m_close_from_tundev_flag){
        m_close_cb(this);
    }    
}

void TUNSession::recv_ack_cmd(int ack_count){
    Session::recv_ack_cmd(ack_count);

    while(ack_count-- > 0){
        if(!m_wait_ack_handler.empty()){
            m_wait_ack_handler.front()(boost::system::error_code());
            m_wait_ack_handler.pop_front();
        }else{
            break;
        }
    }
    
}

void TUNSession::out_async_send_impl(const std::string_view& data_to_send, SentHandler&& _handler){
    if(m_service->is_use_pipeline()){
        auto data_sending_len = data_to_send.length();
        auto self = shared_from_this();
        m_service->session_async_send_to_pipeline(*this, PipelineRequest::DATA, data_to_send,
         [this, self, _handler, data_sending_len](const boost::system::error_code error) {
            reset_udp_timeout();
            if (error) {
                output_debug_info_ec(error);
                destroy();
            }else{
                if(get_sent_len() == 0){
                    output_debug_info();
                    _log_with_date_time(to_string(m_local_addr.port()) + " session_id: " + to_string(get_session_id()) + 
                        " inc_sent_len from 0 to size: " + to_string(data_sending_len), Log::INFO);
                }
                inc_sent_len(data_sending_len);
                
                if(!is_udp_forward_session()){
                    if(!get_pipeline_component().pre_call_ack_func()){
                        m_wait_ack_handler.emplace_back(_handler);
                        _log_with_endpoint_DEBUG(m_local_addr, "session_id: " + to_string(get_session_id()) + " cannot TUNSession::out_async_send ! Is waiting for ack");
                        return;
                    }
                    _log_with_endpoint_DEBUG(m_local_addr, "session_id: " + to_string(get_session_id()) + " permit to TUNSession::out_async_send ! ack:" + to_string(get_pipeline_component().pipeline_ack_counter));
                }
            }
            _handler(error);         
        });
    }else{
        m_sending_data_cache.push_data([&](boost::asio::streambuf& buf){streambuf_append(buf, data_to_send);}, move(_handler));
    }
}
void TUNSession::out_async_send(const uint8_t* _data, size_t _length, SentHandler&& _handler){

    if(m_sending_len <= 100){
        output_debug_info();
        _log_with_date_time(to_string(m_local_addr.port()) + " session_id: " + to_string(get_session_id()) + 
            " increase sending_len from " + to_string(m_sending_len) + " size: " + to_string(_length), Log::INFO);
    }
    m_sending_len += _length;

    if(!m_connected){
        if(m_send_buf.size() < numeric_limits<uint16_t>::max()){
            if(is_udp_forward_session()){
                UDPPacket::generate(m_send_buf, get_config().get_tun().redirect_local ? get_redirect_local_remote_addr() : m_remote_addr_udp, 
                    string_view((const char*)_data, _length));
            }else{
                streambuf_append(m_send_buf, _data, _length);
            }
            m_wait_connected_handler.emplace_back(_handler);
        }else{
            output_debug_info();
            destroy();
        }
    }else{     
        if(is_udp_forward_session()){
            m_send_buf.consume(m_send_buf.size());
            UDPPacket::generate(m_send_buf, get_config().get_tun().redirect_local ? get_redirect_local_remote_addr() : m_remote_addr_udp, 
                string_view((const char*)_data, _length));
            out_async_send_impl(streambuf_to_string_view(m_send_buf), move(_handler));
        }else{
            out_async_send_impl(string_view((const char*)_data, _length), move(_handler));
        }        
    }
}

void TUNSession::try_out_async_read(){
    if(is_destroyed()){
        return;
    }
    
    if(m_service->is_use_pipeline() && !is_udp_forward_session()){
        auto self = shared_from_this();
        m_service->session_async_send_to_pipeline(*this, PipelineRequest::ACK, "", 
          [this, self](const boost::system::error_code error) {
            if (error) {
                output_debug_info_ec(error);
                destroy();
                return;
            }

            out_async_read();
        }, m_ack_count);
        m_ack_count = 0;
    }else{
        out_async_read();
    }
    
}
void TUNSession::recv_buf_ack_sent(uint16_t _length){
    assert(!is_udp_forward_session());
    m_recv_buf_ack_length -= _length;

    if(m_service->is_use_pipeline() && m_recv_buf_ack_length <= 0){
        if(get_pipeline_component().is_write_close_future()){
            output_debug_info();
            destroy();
            return;
        }

        get_pipeline_component().set_async_writing_data(false);
    }
}

void TUNSession::recv_buf_consume(uint16_t _length){
    assert(!is_udp_forward_session());
    m_recv_buf.consume(_length);
    if(m_recv_buf.size() == 0){
        try_out_async_read();
    }
}

size_t TUNSession::parse_udp_packet_data(const string_view& data){

    string_view parse_data(data);
    size_t parsed_size = 0;
    for(;;){
        if(parse_data.empty()){
            break;
        }

        // parse trojan protocol
        UDPPacket packet;
        size_t packet_len = 0;
        if(!packet.parse(parse_data, packet_len)){
            if(parse_data.length() > numeric_limits<uint16_t>::max()){
                _log_with_endpoint(get_udp_local_endpoint(), "[tun] error UDPPacket.parse! destroy it.", Log::ERROR);
                destroy();
                break;
            }

            _log_with_endpoint(get_udp_local_endpoint(), "[tun] UDPPacket.parse failed! Might need to read more...", Log::WARN);
            break;
        }

        if(m_write_to_lwip(this, &packet.payload) < 0){
            output_debug_info();
            destroy();
            break;
        }

        parsed_size += packet_len;
        parse_data = parse_data.substr(packet_len);
    }

    return parsed_size;
}

void TUNSession::out_async_read() {
    if(m_service->is_use_pipeline()){    
        get_pipeline_component().get_pipeline_data_cache().async_read([this](const string_view &data, int ack_count) {
            inc_recv_len(data.length());

            if(is_udp_forward_session()){
                
                reset_udp_timeout();

                if(m_recv_buf.size() == 0){
                    auto parsed = parse_udp_packet_data(data);
                    if(parsed < data.length()){
                        streambuf_append(m_recv_buf, data.substr(parsed));
                    }
                }else{
                    streambuf_append(m_recv_buf, data);
                    auto parsed = parse_udp_packet_data(m_recv_buf);
                    m_recv_buf.consume(parsed);
                }                

                try_out_async_read();
            }else{
                m_ack_count += ack_count;
                streambuf_append(m_recv_buf, data);
                m_recv_buf_ack_length += data.length();

                get_pipeline_component().set_async_writing_data(true);
                if(m_write_to_lwip(this, nullptr) < 0){
                    output_debug_info();
                    destroy();
                }
            }
            
        });
    }else{
        m_recv_buf.begin_read(__FILE__, __LINE__);
        auto self = shared_from_this();
        m_out_socket.async_read_some(m_recv_buf.prepare(Session::MAX_BUF_LENGTH), [this, self](const boost::system::error_code error, size_t length) {
            m_recv_buf.end_read();
            if (error) {
                output_debug_info_ec(error);
                destroy();
                return;
            }
            m_recv_buf.commit(length);
            inc_recv_len(length);

            if(is_udp_forward_session()){
                reset_udp_timeout();    
                auto parsed = parse_udp_packet_data(m_recv_buf);
                m_recv_buf.consume(parsed);

                try_out_async_read();
            }else{
                m_recv_buf_ack_length += length;

                if(m_write_to_lwip(this, nullptr) < 0){
                    output_debug_info();
                    destroy();
                }
            }            
        });
    }
}

bool TUNSession::try_to_process_udp(const boost::asio::ip::udp::endpoint& _local, 
        const boost::asio::ip::udp::endpoint& _remote, const uint8_t* payload, size_t payload_length){
            
    if(is_udp_forward_session()){
        if(_local == m_local_addr_udp && _remote == m_remote_addr_udp){
            out_async_send(payload, payload_length, [](boost::system::error_code){});
            return true;
        }
    }

    return false;
}

