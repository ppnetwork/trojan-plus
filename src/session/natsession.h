/*
 * This file is part of the Trojan Plus project.
 * Trojan is an unidentifiable mechanism that helps you bypass GFW.
 * Trojan Plus is derived from original trojan project and writing 
 * for more experimental features.
 * Copyright (C) 2017-2020  The Trojan Authors.
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

#ifndef _NATSESSION_H_
#define _NATSESSION_H_

#include <boost/asio/ssl.hpp>
#include "clientsession.h"

class NATSession : public ClientSession {
protected:
    virtual std::pair<std::string, uint16_t> get_target_endpoint();
    void in_recv(const std::string_view &data) override;
    void in_sent() override;

public:
    NATSession(Service* _service, const Config& config, boost::asio::ssl::context &ssl_context);
    void start() override;
};

#endif // _NATSESSION_H_
