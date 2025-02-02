#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "profile_infos.hpp"
#include "network.hpp"
#include "party.hpp"
#include "scheduler.hpp"

#include <utils/nt.hpp>
#include <utils/properties.hpp>
#include <utils/concurrency.hpp>

#include "../steam/steam.hpp"
#include <utils/io.hpp>

#include "game/utils.hpp"

namespace profile_infos
{
	namespace
	{
		using profile_map = std::unordered_map<uint64_t, profile_info>;
		utils::concurrency::container<profile_map> profile_mapping{};

		std::optional<profile_info> load_profile_info()
		{
			std::string data{};
			if (!utils::io::read_file("players/user/profile_info", &data))
			{
				return {};
			}

			profile_info info{};
			constexpr auto version_size = sizeof(info.version);

			if (data.size() < sizeof(version_size))
			{
				return {};
			}

			memcpy(&info.version, data.data(), version_size);
			info.ddl.assign(data.begin() + version_size, data.end());

			return {std::move(info)};
		}

		void send_profile_info(const game::netadr_t& address, const std::string& buffer)
		{
			network::send(address, "profileInfo", buffer);
		}

		void distribute_profile_info(const uint64_t user_id, const profile_info& info)
		{
			if (user_id == steam::SteamUser()->GetSteamID().bits)
			{
				return;
			}

			utils::byte_buffer buffer{};
			buffer.write(user_id);
			info.serialize(buffer);

			const std::string data = buffer.move_buffer();

			game::foreach_connected_client([&](const game::client_s& client)
			{
				send_profile_info(client.address, data);
			});
		}

		void schedule_pcache_update()
		{
			static std::atomic_bool update_triggered{false};
			if (game::is_server() || update_triggered.exchange(true))
			{
				return;
			}

			scheduler::once([]
			{
				game::PCache_DeleteEntries(game::CONTROLLER_INDEX_FIRST);
				update_triggered = false;
			}, scheduler::main, 5s);
		}

		std::unordered_set<uint64_t> get_connected_client_xuids()
		{
			std::unordered_set<uint64_t> connected_clients{};
			connected_clients.reserve(game::get_max_client_count());

			game::foreach_connected_client([&](const game::client_s& client)
			{
				connected_clients.emplace(client.xuid);
			});

			return connected_clients;
		}

		void clean_cached_profile_infos()
		{
			if (!game::is_server_running())
			{
				return;
			}

			const auto xuids = get_connected_client_xuids();

			profile_mapping.access([&](profile_map& profiles)
			{
				for (auto i = profiles.begin(); i != profiles.end();)
				{
					if (xuids.contains(i->first))
					{
						++i;
					}
					else
					{
						i = profiles.erase(i);
					}
				}
			});
		}
	}

	profile_info::profile_info(utils::byte_buffer& buffer)
	{
		this->version = buffer.read<int32_t>();
		this->ddl = buffer.read_string();
	}

	void profile_info::serialize(utils::byte_buffer& buffer) const
	{
		buffer.write(this->version);
		buffer.write_string(this->ddl);
	}

	void add_profile_info(const uint64_t user_id, const profile_info& info)
	{
		if (user_id == steam::SteamUser()->GetSteamID().bits)
		{
			return;
		}

		profile_mapping.access([&](profile_map& profiles)
		{
			profiles[user_id] = info;
		});

		schedule_pcache_update();
	}

	void distribute_profile_info_to_user(const game::netadr_t& addr, const uint64_t user_id, const profile_info& info)
	{
		utils::byte_buffer buffer{};
		buffer.write(user_id);
		info.serialize(buffer);

		send_profile_info(addr, buffer.get_buffer());
	}

	void distribute_profile_infos_to_user(const game::netadr_t& addr)
	{
		profile_mapping.access([&](const profile_map& profiles)
		{
			for (const auto& entry : profiles)
			{
				distribute_profile_info_to_user(addr, entry.first, entry.second);
			}
		});

		if (!game::is_server())
		{
			const auto info = get_profile_info();
			if (info)
			{
				distribute_profile_info_to_user(addr, steam::SteamUser()->GetSteamID().bits, *info);
			}
		}
	}

	void add_and_distribute_profile_info(const game::netadr_t& addr, const uint64_t user_id, const profile_info& info)
	{
		distribute_profile_infos_to_user(addr);

		add_profile_info(user_id, info);
		distribute_profile_info(user_id, info);
	}

	void clear_profile_infos()
	{
		profile_mapping.access([&](profile_map& profiles)
		{
			profiles = {};
		});
	}

	std::optional<profile_info> get_profile_info()
	{
		return load_profile_info();
	}

	std::optional<profile_info> get_profile_info(const uint64_t user_id)
	{
		printf("Requesting profile info: %llX\n", user_id);

		if (user_id == steam::SteamUser()->GetSteamID().bits)
		{
			return get_profile_info();
		}

		return profile_mapping.access<std::optional<profile_info>>([user_id](const profile_map& profiles)
		{
			std::optional<profile_info> result{};

			const auto profile_entry = profiles.find(user_id);
			if (profile_entry != profiles.end())
			{
				result = profile_entry->second;
			}

			return result;
		});
	}

	void update_profile_info(const profile_info& info)
	{
		std::string data{};
		data.reserve(4 + info.ddl.size());

		data.append(reinterpret_cast<const char*>(&info.version), sizeof(info.version));
		data.append(info.ddl);

		utils::io::write_file("players/user/profile_info", data);
	}

	struct component final : generic_component
	{
		void post_unpack() override
		{
			scheduler::loop(clean_cached_profile_infos, scheduler::main, 5s);

			if (game::is_client())
			{
				network::on("profileInfo", [](const game::netadr_t& server, const network::data_view& data)
				{
					if (!party::is_host(server))
					{
						return;
					}

					utils::byte_buffer buffer(data);
					const auto user_id = buffer.read<uint64_t>();
					const profile_info info(buffer);

					add_profile_info(user_id, info);
				});
			}
		}
	};
}

REGISTER_COMPONENT(profile_infos::component)
