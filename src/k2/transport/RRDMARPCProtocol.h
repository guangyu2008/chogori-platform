/*
MIT License

Copyright(c) 2020 Futurewei Cloud

    Permission is hereby granted,
    free of charge, to any person obtaining a copy of this software and associated documentation files(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions :

    The above copyright notice and this permission notice shall be included in all copies
    or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS",
    WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER
    LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#pragma once
// third-party
#include <seastar/net/rdma.hh>

// k2
#include "IRPCProtocol.h"
#include "VirtualNetworkStack.h"
#include "RPCProtocolFactory.h"
#include "RRDMARPCChannel.h"
#include "RPCHeader.h"

namespace k2 {

// RRDMARPCProtocol is a protocol which uses the currently configured RRDMA stack, with responsibility to:
// - listen for incoming RRDMA connections
// - create outgoing RRDMA connections when asked to send messages
// - receive incoming messages and pass them on to the message observer for the protocol
// NB, the class is meant to be used as a distributed<> container
class RRDMARPCProtocol: public IRPCProtocol {
public: // types
    // Convenience builder which opens an RRDMA listener across all cores
    static RPCProtocolFactory::BuilderFunc_t builder(VirtualNetworkStack::Dist_t& vnet);

    // The official protocol name supported for communications over RRDMARPC channels
    static const String proto;

public: // lifecycle
    // Construct the protocol with a vnet which supports RRDMA
    RRDMARPCProtocol(VirtualNetworkStack::Dist_t& vnet);

    // Destructor
    virtual ~RRDMARPCProtocol();

public: // API

    // This method creates an endpoint for a given URL. The endpoint is needed in order to
    // 1. obtain protocol-specific payloads
    // 2. send messages.
    // returns blank pointer if we failed to parse the url or if the protocol is not supported
    std::unique_ptr<TXEndpoint> getTXEndpoint(String url) override;

    // Invokes the remote rpc for the given verb with the given payload. This is an asyncronous API. No guarantees
    // are made on the delivery of the payload after the call returns.
    // This is a lower-level API which is useful for sending messages that do not expect replies.
    // The RPC message is configured with the given metadata
    void send(Verb verb, std::unique_ptr<Payload> payload, TXEndpoint& endpoint, MessageMetadata metadata) override;

    // Returns the endpoint where this protocol accepts incoming connections.
    seastar::lw_shared_ptr<TXEndpoint> getServerEndpoint() override;

public: // distributed<> interface
    // iface: called by seastar's distributed mechanism when stop() is invoked on the distributed container.
    // The method's returned future completes once all channels had a chance to complete a graceful shutdown
    seastar::future<> stop() override;

    // Should be called by user when all distributed objects have been created
    void start() override;

private: // methods
    // utility method which ew use to obtain a connection(either existing or new) for the given endpoint
    seastar::lw_shared_ptr<RRDMARPCChannel> _getOrMakeChannel(TXEndpoint& endpoint);

    // process a new channel creation
    seastar::lw_shared_ptr<RRDMARPCChannel>
    _handleNewChannel(std::unique_ptr<seastar::rdma::RDMAConnection> rconn, TXEndpoint endpoint);

    // Helper method to create an TXEndpoint from an rdma address
    TXEndpoint _endpointFromAddress(seastar::rdma::EndPoint addr);

private: // fields
    // we use this flag to signal exit
    bool _stopped;
    // our listener
    seastar::rdma::RDMAListener _listener;
    seastar::future<> _listenerClosed = seastar::make_ready_future();

    // the endpoint version of the address we're listening on
    seastar::lw_shared_ptr<TXEndpoint> _svrEndpoint;
    // the underlying RRDMA channels we're dealing with
    std::unordered_map<TXEndpoint, seastar::lw_shared_ptr<RRDMARPCChannel>> _channels;

private: // not needed
    RRDMARPCProtocol() = delete;
    RRDMARPCProtocol(const RRDMARPCProtocol& o) = delete;
    RRDMARPCProtocol(RRDMARPCProtocol&& o) = delete;
    RRDMARPCProtocol &operator=(const RRDMARPCProtocol& o) = delete;
    RRDMARPCProtocol &operator=(RRDMARPCProtocol&& o) = delete;

}; // class RRDMARPCProtocol

} // namespace k2
