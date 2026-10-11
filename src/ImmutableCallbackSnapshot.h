#pragma once

#include <memory>
#include <utility>

namespace dvb
{
	// Caller owns the publication mutex. Retirements copy only shared ownership;
	// registration performs every fallible map/callable copy before committing.
	template<class Map>
	void UpdateCallbackSnapshot(std::shared_ptr<const Map>& current,
		const typename Map::key_type& key, typename Map::mapped_type callback)
	{
		auto candidate = current ? std::make_shared<Map>(*current) : std::make_shared<Map>();
		candidate->insert_or_assign(key, std::move(callback));
		current = std::move(candidate);
	}
}
