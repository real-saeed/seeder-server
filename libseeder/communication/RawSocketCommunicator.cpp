#include "RawSocketCommunicator.h"

#include <mutex>

#include <evpp/udp/udp_server.h>
#include <evpp/event_loop_thread_pool.h>
#include <evpp/event_loop.h>

#include "SpdLogger.h"
#include "Response_generated.h"

std::mutex client_manager_mutex;

RawSocketCommunicator::RawSocketCommunicator(InMemoryClientManager* client_manager, const Argument& args)
: beginning_port(args.beginning_port)
, client_manager(client_manager)
, peer_availability_interval(args.client_ping_interval)
{
}

void RawSocketCommunicator::run()
{
    const int thread_num = 5;
    std::vector<int> ports;
    for (int i = 0; i < 5; ++i)
        ports.push_back(beginning_port + i);

	evpp::udp::Server server;
	server.SetThreadDispatchPolicy(evpp::ThreadDispatchPolicy::kIPAddressHashing);
	server.SetMessageHandler([this](evpp::EventLoop* loop, evpp::udp::MessagePtr& msg) {
        auto request = parse_datagram(msg);
        logging::log()->info("New request of type {} with ID {} from {}",
            request_type_to_string(request->request_type()), request->id(),
            msg->remote_ip());

        switch (request->request_type())
        {
        case Seeder::RequestType_HelloRequest:
            handle_hello_request(request, msg);
            break;
        case Seeder::RequestType_GetElitedPeersRequest:
            handle_get_elited_peers_request(request, msg);
            break;
        case Seeder::RequestType_GetPeersByLastAliveRequest:
            handle_get_peers_by_last_alive_request(request, msg);
            break;
        case Seeder::RequestType_PeerStatusRequest:
            handle_peer_status_request(request, msg);
            break;
        case Seeder::RequestType_ByeRequest:
            handle_bye_request(request, msg);
            break;
        default:
            logging::log()->warn("Unknown request type {}", request->request_type());
            break;
        }
		});
	server.Init(ports);
    if (server.Start()) {
        logging::log()->info("Server successfully started.");
    }
    else {
        logging::log()->critical("Server failed to start.");
        exit(EXIT_FAILURE);
    }

	evpp::EventLoop loop;
	std::shared_ptr<evpp::EventLoopThreadPool> tpool(new evpp::EventLoopThreadPool(&loop, thread_num));
	tpool->Start(true);
	server.SetEventLoopThreadPool(tpool);
	loop.Run();
	server.Stop(true);
	tpool->Stop(true);
}

const Seeder::Request* RawSocketCommunicator::parse_datagram(evpp::udp::MessagePtr& msg)
{
    const uint16_t incoming_data_size = msg->ReadInt16();
    const char* data = msg->data();

    return Seeder::GetRequest(data);
}

void RawSocketCommunicator::handle_hello_request(
    const Seeder::Request* request, evpp::udp::MessagePtr& msg)
{
    flatbuffers::FlatBufferBuilder builder(100);
    flatbuffers::Offset<Seeder::HelloResponse> response;
    bool added_successfully;
    {
        std::lock_guard<std::mutex> guard(client_manager_mutex);
        added_successfully = client_manager->add({ request->request_as_HelloRequest()->address()->str() });
    }
    if (added_successfully)
        response = Seeder::CreateHelloResponse(builder,
            Seeder::HelloRequestResult::HelloRequestResult_REGISTERED_SUCCESSFULLY,
            peer_availability_interval.count());
    else
        response = Seeder::CreateHelloResponse(builder, Seeder::HelloRequestResult::HelloRequestResult_ALREADY_REGISTERED);

    auto response_builder = Seeder::ResponseBuilder(builder);
    response_builder.add_id(request->id());

    response_builder.add_response(response.Union());
    response_builder.add_response_type(Seeder::ResponseType_HelloResponse);

	auto orc = response_builder.Finish();
	builder.Finish(orc);
	const uint8_t* buffer = builder.GetBufferPointer();
	const size_t size = builder.GetSize();

    if (evpp::udp::SendMessage(msg->sockfd(), msg->remote_addr(),
        reinterpret_cast<const char*>(buffer), size))
        logging::log()->info("Response with {} bytes successfully sent", size);
    else
        logging::log()->critical("Failed to send the response to the peer");
}

void RawSocketCommunicator::handle_get_elited_peers_request(
    const Seeder::Request* request, evpp::udp::MessagePtr& msg)
{
    flatbuffers::FlatBufferBuilder builder(1000);

	std::vector<flatbuffers::Offset<flatbuffers::String>> active_peers;
    {
        std::lock_guard<std::mutex> guard(client_manager_mutex);
		const auto peers = client_manager->get_elited_peers(
			request->request_as_GetElitedPeersRequest()->number_of_peers());

		for (const auto& peer : peers) {
			active_peers.push_back(builder.CreateString(peer->get_address()));
		}
    }
    auto response = Seeder::CreateGetElitedPeersResponse(builder, builder.CreateVector(active_peers));

    auto response_builder = Seeder::ResponseBuilder(builder);
    response_builder.add_id(request->id());
    response_builder.add_response(response.Union());
    response_builder.add_response_type(Seeder::ResponseType_GetElitedPeersResponse);

	auto orc = response_builder.Finish();
	builder.Finish(orc);
	const uint8_t* buffer = builder.GetBufferPointer();
	const size_t size = builder.GetSize();

    if (!evpp::udp::SendMessage(msg->sockfd(), msg->remote_addr(), 
        reinterpret_cast<const char*>(buffer), size))
        logging::log()->critical("Failed to send {} bytes to client.", size);
}

void RawSocketCommunicator::handle_peer_status_request(const Seeder::Request* request,
    evpp::udp::MessagePtr& msg)
{
    const auto status_request = request->request_as_PeerStatusRequest();
    std::string connections_string;
    for (size_t i = 0; i < status_request->peer_current_connections()->size(); ++i) {
        const std::string connection = status_request->peer_current_connections()->GetAsString(i)->str();
        connections_string += connection + ", ";
    }

    const time_t last_alive = status_request->last_alive();
    {
        std::lock_guard<std::mutex> guard(client_manager_mutex);

		client_manager->touch(status_request->address()->str(), last_alive);
		client_manager->set_number_of_connections_of(status_request->address()->str(),
			status_request->peer_current_connections()->size());
    }

    logging::log()->info("Peer {} is alive ({}) and has {} connections with: {}",
        status_request->address()->str(),
        last_alive,
        status_request->peer_current_connections()->size(),
        connections_string);
}

void RawSocketCommunicator::handle_bye_request(const Seeder::Request* request, evpp::udp::MessagePtr& msg)
{
    auto bye_request = request->request_as_ByeRequest();
    {
        std::lock_guard<std::mutex> guard(client_manager_mutex);
        client_manager->remove(bye_request->address()->str());
    }
}

void RawSocketCommunicator::handle_get_peers_by_last_alive_request(const Seeder::Request* request, evpp::udp::MessagePtr& msg)
{
    flatbuffers::FlatBufferBuilder builder(1000);

    std::vector<flatbuffers::Offset<flatbuffers::String>> active_peers;
    {
        std::lock_guard<std::mutex> guard(client_manager_mutex);
		const auto peers = client_manager->get_alive_peers_since(
			request->request_as_GetPeersByLastAliveRequest()->last_alive_since());

		for (const auto& peer : peers) {
			active_peers.push_back(builder.CreateString(peer->get_address()));
		}
    }

    logging::log()->info("Getting alive peers since: {}", 
        request->request_as_GetPeersByLastAliveRequest()->last_alive_since());

    auto response = Seeder::CreateGetAlivePeersResponse(builder, builder.CreateVector(active_peers));

    auto response_builder = Seeder::ResponseBuilder(builder);
    response_builder.add_id(request->id());
    response_builder.add_response(response.Union());
    response_builder.add_response_type(Seeder::ResponseType_GetAlivePeersResponse);

	auto orc = response_builder.Finish();
	builder.Finish(orc);
	const uint8_t* buffer = builder.GetBufferPointer();
	const size_t size = builder.GetSize();

    if (!evpp::udp::SendMessage(msg->sockfd(), msg->remote_addr(), 
        reinterpret_cast<const char*>(buffer), size))
        logging::log()->critical("Failed to send {} bytes to client.", size);
}

std::string RawSocketCommunicator::request_type_to_string(Seeder::RequestType request_type)
{
    switch (request_type)
    {
    case Seeder::RequestType_HelloRequest:
        return "HELLO";
    case Seeder::RequestType_PeerStatusRequest:
        return "PEER_STATUS";
    case Seeder::RequestType_GetElitedPeersRequest:
        return "GET_PEERS";
    case Seeder::RequestType_GetPeersByLastAliveRequest:
        return "ALIVE_PEERS";
        break;
    case Seeder::RequestType_NONE:
    default:
        break;
    }

    return "INVALID";
}
