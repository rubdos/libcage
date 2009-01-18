/*
 * Copyright (c) 2009, Yuuki Takano (ytakanoster@gmail.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the writers nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "dtun.hpp"

#include <boost/foreach.hpp>

namespace libcage {
        const int       dtun::num_find_node = 6;
        const int       dtun::max_query     = 3;
        const int       dtun::query_timeout = 2;

        void
        dtun::timer_query::operator() ()
        {
                query_ptr q = p_dtun->m_query[nonce];
                timer_ptr t = q->timers[id];

                q->sent.insert(id);
                q->num_query--;
                q->timers.erase(id);

                p_dtun->m_peers.add_timeout(id.id);
                p_dtun->remove(*id.id);

                // send find node
                p_dtun->send_find(q);
        }

        dtun::dtun(const uint160_t &id, timer &t, peers &p,
                   const natdetector &nat, udphandler &udp) :
                rttable(id, t, p), m_id(id), m_timer(t), m_peers(p), m_nat(nat),
                m_udp(udp)
        {

        }

        dtun::~dtun()
        {

        }

        void
        dtun::send_ping(cageaddr &dst, uint32_t nonce)
        {
                msg_dtun_ping ping;

                memset(&ping, 0, sizeof(ping));

                ping.hdr.magic = htons(MAGIC_NUMBER);
                ping.hdr.ver   = htons(CAGE_VERSION);
                ping.hdr.type  = htons(type_dtun_ping);

                m_id.to_binary(ping.hdr.src, sizeof(ping.hdr.src));
                dst.id->to_binary(ping.hdr.dst, sizeof(ping.hdr.dst));

                ping.nonce = htonl(nonce);

                if (dst.domain == domain_inet) {
                        sockaddr* saddr;
                        saddr = (sockaddr*)boost::get<sockaddr_in*>(dst.saddr);
                        m_udp.sendto(&ping, sizeof(ping), saddr,
                                     sizeof(sockaddr_in));
                } else {
                        sockaddr* saddr;
                        saddr = (sockaddr*)boost::get<sockaddr_in6*>(dst.saddr);
                        m_udp.sendto(&ping, sizeof(ping), saddr,
                                     sizeof(sockaddr_in6));
                }
        }

        void
        dtun::recv_ping(void *msg, sockaddr *from, int fromlen)
        {
                msg_dtun_ping       *ping = (msg_dtun_ping*)msg;
                msg_dtun_ping_reply  reply;
                uint160_t            fromid;

                fromid.from_binary(ping->hdr.dst, sizeof(ping->hdr.dst));
                if (fromid != m_id)
                        return;


                // send ping reply
                memset(&reply, 0, sizeof(reply));

                reply.hdr.magic = htons(MAGIC_NUMBER);
                reply.hdr.ver   = htons(CAGE_VERSION);
                reply.hdr.type  = htons(type_dtun_ping_reply);

                m_id.to_binary(reply.hdr.src, sizeof(reply.hdr.src));
                memcpy(reply.hdr.dst, ping->hdr.src, sizeof(reply.hdr.dst));

                reply.nonce = ping->nonce;

                m_udp.sendto(&reply, sizeof(reply), from, fromlen);


                // add to peers
                cageaddr addr = new_cageaddr(&ping->hdr, from);
                m_peers.add_node(addr);
        }

        void
        dtun::recv_ping_reply(void *msg, sockaddr *from, int fromlen)
        {
                msg_dtun_ping_reply *reply = (msg_dtun_ping_reply*)msg;
                uint160_t            fromid;

                fromid.from_binary(reply->hdr.dst, sizeof(reply->hdr.dst));
                if (fromid != m_id)
                        return;

                cageaddr addr = new_cageaddr(&reply->hdr, from);

                rttable::recv_ping_reply(addr, ntohl(reply->nonce));


                // add to peers
                m_peers.add_node(addr);
        }

        void
        dtun::find_node(std::string host, int port, callback_find_node func)
        {
                sockaddr_storage saddr;

                if (! m_udp.get_sockaddr(&saddr, host, port))
                        return;

                // initialize query
                query_ptr q(new query);

                q->dst           = m_id;
                q->num_query     = 1;
                q->is_find_value = false;
                q->func          = func;

                // add my id
                _id i;
                i.id = id_ptr(new uint160_t);
                *i.id = m_id;

                q->sent.insert(i);

                uint32_t nonce;
                do {
                        nonce = mrand48();
                } while (m_query.find(nonce) != m_query.end());

                q->nonce = nonce;
                m_query[nonce] = q;


                // start timer
                timeval   tval;
                timer_ptr t(new timer_query);
                id_ptr    zero(new uint160_t);
                _id       zero_id;

                zero->fill_zero();
                zero_id.id = zero;

                t->nonce  = q->nonce;
                t->id     = zero_id;
                t->p_dtun = this;

                tval.tv_sec  = query_timeout;
                tval.tv_usec = 0;

                q->timers[zero_id] = t;

                m_timer.set_timer(t.get(), &tval);


                // send find node
                cageaddr addr;
                addr.id = zero;

                addr.domain = m_udp.get_domain();
                if (addr.domain == domain_inet) {
                        in_ptr in(new sockaddr_in);
                        memcpy(in.get(), &saddr, sizeof(sockaddr_in));
                        addr.saddr = in;
                } else if (addr.domain == domain_inet6) {
                        in_ptr in6(new sockaddr_in);
                        memcpy(in6.get(), &saddr, sizeof(sockaddr_in6));
                        addr.saddr = in6;
                }

                send_find_node(addr, q);
        }

        void
        dtun::find_nv(const uint160_t &dst, callback_func func,
                      bool is_find_value)
        {
                query_ptr q(new query);

                lookup(dst, num_find_node, q->nodes);

                if (q->nodes.size()) {
                        if (is_find_value) {
                                callback_find_value f;
                                f = boost::get<callback_find_value>(func);
                                cageaddr addr;
                                f(false, addr);
                        } else {
                                callback_find_node f;
                                f = boost::get<callback_find_node>(func);
                                f(q->nodes);
                        }
                        return;
                }

                q->dst           = dst;
                q->num_query     = 0;
                q->is_find_value = is_find_value;
                q->func          = func;

                // add my id
                _id i;
                i.id = id_ptr(new uint160_t);
                *i.id = m_id;

                q->sent.insert(i);


                uint32_t nonce;
                do {
                        nonce = mrand48();
                } while (m_query.find(nonce) != m_query.end());

                q->nonce = nonce;
                m_query[nonce] = q;


                send_find(q);
        }

        void
        dtun::find_node(const uint160_t &dst, callback_find_node func)
        {
                find_nv(dst, func, false);
        }

        void
        dtun::find_value(const uint160_t &dst, callback_find_value func)
        {
                // TODO: check local cache

                find_nv(dst, func, true);
        }

        void
        dtun::send_find(query_ptr q)
        {
                BOOST_FOREACH(cageaddr &addr, q->nodes) {
                        if (q->num_query >= max_query) {
                                break;
                        }

                        _id i;
                        i.id = addr.id;

                        if (q->sent.find(i) != q->sent.end()) {
                                continue;
                        }

                        // start timer
                        timeval   tval;
                        timer_ptr t(new timer_query);

                        t->nonce  = q->nonce;
                        t->id     = i;
                        t->p_dtun = this;

                        tval.tv_sec  = query_timeout;
                        tval.tv_usec = 0;

                        q->timers[i] = t;

                        m_timer.set_timer(t.get(), &tval);


                        // send find node
                        if (q->is_find_value) {
                                send_find_value(addr, q);
                        } else {
                                send_find_node(addr, q);
                        }
                }

                if (q->num_query == 0) {
                        // call callback functions
                        if (q->is_find_value) {
                                cageaddr addr;
                                callback_find_value func;
                                func = boost::get<callback_find_value>(q->func);
                                func(false, addr);
                        } else {
                                callback_find_node func;
                                func = boost::get<callback_find_node>(q->func);
                                func(q->nodes);
                        }

                        // stop all timers
                        std::map<_id, timer_ptr>::iterator it;
                        for (it = q->timers.begin(); it != q->timers.end();
                             ++it) {
                                m_timer.unset_timer(it->second.get());
                        }

                        // remove query
                        m_query.erase(q->nonce);
                }
        }

#define SEND_FIND(MSG, TYPE, DST, Q)                                    \
        do {                                                            \
                MSG msg;                                                \
                                                                        \
                memset(&msg, 0, sizeof(msg));                           \
                                                                        \
                msg.hdr.magic = htons(MAGIC_NUMBER);                    \
                msg.hdr.ver   = htons(CAGE_VERSION);                    \
                msg.hdr.type  = htons(TYPE);                            \
                                                                        \
                m_id.to_binary(msg.hdr.src, sizeof(msg.hdr.src));       \
                DST.id->to_binary(msg.hdr.dst, sizeof(msg.hdr.dst));    \
                                                                        \
                msg.nonce  = htonl(Q->nonce);                           \
                msg.domain = htons(m_udp.get_domain());                 \
                                                                        \
                if (m_nat.is_global()) {                                \
                        msg.state = htons(state_global);                \
                } else {                                                \
                        msg.state = htons(state_nat);                   \
                }                                                       \
                                                                        \
                Q->dst.to_binary(msg.id, sizeof(msg.id));               \
                                                                        \
                if (DST.domain == domain_inet) {                        \
                        in_ptr in = boost::get<in_ptr>(DST.saddr);      \
                        m_udp.sendto(&msg, sizeof(msg),                 \
                                     (sockaddr*)in.get(),               \
                                     sizeof(sockaddr_in));              \
                } else if (DST.domain == domain_inet6) {                \
                        in6_ptr in6 = boost::get<in6_ptr>(DST.saddr);   \
                        m_udp.sendto(&msg, sizeof(msg),                 \
                                     (sockaddr*)in6.get(),              \
                                     sizeof(sockaddr_in6));             \
                }                                                       \
        } while (0);

        void
        dtun::send_find_node(cageaddr &dst, query_ptr q)
        {
                SEND_FIND(msg_dtun_find_node, type_dtun_find_node, dst, q);
        }

        void
        dtun::send_find_value(cageaddr &dst, query_ptr q)
        {
                SEND_FIND(msg_dtun_find_value, type_dtun_find_value, dst, q);
        }
}