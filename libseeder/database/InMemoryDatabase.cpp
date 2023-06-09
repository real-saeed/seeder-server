#include "InMemoryDatabase.h"

#include <iterator>
#include "SpdLogger.h"

bool InMemoryDatabase::add_client(Client&& client)
{
	if (active_clients.find(client.get_address()) != active_clients.end())
	{
		logging::log()->warn("Trying to add an existing client with address {}", client.get_address());
		return false;
	}

	logging::log()->info("Client {} added to Client manager.", client.get_address());

	auto shared_client = std::make_shared<Client>(std::move(client));
	active_clients.insert(std::make_pair(shared_client->get_address(), shared_client));

	auto inserted_at = last_alive_ordered_client_set.insert(shared_client);
	shared_client->set_last_alive_set_inserted_at_iterator(inserted_at);

	auto inserted_at_elite_set = elite_client_set.insert(shared_client);
	shared_client->set_elite_set_inserted_at_iterator(inserted_at_elite_set);
	return true;
}

void InMemoryDatabase::remove_client(const std::string& address)
{
	auto client = active_clients.find(address);

	if (client == active_clients.end())
	{
		logging::log()->warn("Trying to remove a non-existing client with address {}", address);
		return;
	}

	logging::log()->info("Client {} removed from Client manager.", address);
	last_alive_ordered_client_set.erase(client->second->get_last_alive_set_inserted_at_iterator());
	elite_client_set.erase(client->second->get_elite_set_inserted_at_iterator());
	active_clients.erase(address);
}

void InMemoryDatabase::touch_client(const std::string& address, time_t alive_timestamp)
{
	auto client = active_clients.find(address);

	if (client == active_clients.end())
	{
		logging::log()->warn("Trying to touch(update alive) a non-existing client with address {}", address);
		return;
	}

	client->second->set_last_alive(alive_timestamp);

	// Reorder the client based on new timestamp
	last_alive_ordered_client_set.erase(client->second->get_last_alive_set_inserted_at_iterator());
	auto inserted_at = last_alive_ordered_client_set.insert(client->second);
	client->second->set_last_alive_set_inserted_at_iterator(inserted_at);
}

size_t InMemoryDatabase::get_clients_count() const noexcept
{
	assert(active_clients.size() == last_alive_ordered_client_set.size());
	return active_clients.size();
}

std::vector<SharedClient> InMemoryDatabase::get_elited_peers(size_t count)
{
	std::vector<SharedClient> active;
	std::copy(elite_client_set.cbegin(),
		std::next(elite_client_set.cbegin(), std::min(count, elite_client_set.size())),
		std::back_inserter(active));
	return active;
}

std::vector<SharedClient> InMemoryDatabase::get_alive_peers_since(time_t since)
{
	std::vector<SharedClient> active;

	SharedClient fake_shared = std::make_shared<Client>("");
	fake_shared->set_last_alive(since);
	std::copy(last_alive_ordered_client_set.cbegin(),
		last_alive_ordered_client_set.upper_bound(fake_shared),
		std::back_inserter(active));
	return active;
}

void InMemoryDatabase::set_number_of_connections_of(const std::string& address, size_t n)
{
	auto client = active_clients.find(address);

	if (client == active_clients.end())
	{
		logging::log()->warn("Trying to increment connections of a non-existing client with address {}", address);
		return;
	}

	client->second->set_number_of_connections(n);

	// Reorder the client based on new connection count
	elite_client_set.erase(client->second->get_elite_set_inserted_at_iterator());
	auto inserted_at = elite_client_set.insert(client->second);
	client->second->set_elite_set_inserted_at_iterator(inserted_at);
}


