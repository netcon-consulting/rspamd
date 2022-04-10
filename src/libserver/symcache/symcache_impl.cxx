/*-
 * Copyright 2022 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "symcache_internal.hxx"
#include "unix-std.h"
#include "libutil/cxx/locked_file.hxx"

namespace rspamd::symcache {

INIT_LOG_MODULE_PUBLIC(symcache)

auto symcache::init() -> bool
{
	auto res = true;
	reload_time = cfg->cache_reload_time;

	if (cfg->cache_filename != nullptr) {
		res = load_items();
	}

	/* Deal with the delayed dependencies */
	for (const auto &delayed_dep : *delayed_deps) {
		auto virt_item = get_item_by_name(delayed_dep.from, false);
		auto real_item = get_item_by_name(delayed_dep.from, true);

		if (virt_item == nullptr || real_item == nullptr) {
			msg_err_cache("cannot register delayed dependency between %s and %s: "
						   "%s is missing",
						   delayed_dep.from.data(),
						   delayed_dep.to.data(), delayed_dep.from.data());
		}
		else {
			msg_debug_cache("delayed between %s(%d:%d) -> %s",
					delayed_dep.from.data(),
					real_item->id, virt_item->id,
					delayed_dep.to.data());
			add_dependency(real_item->id, delayed_dep.to, virt_item != real_item ?
														  virt_item->id : -1);
		}
	}

	/* Remove delayed dependencies, as they are no longer needed at this point */
	delayed_deps.reset();


	/* Deal with the delayed conditions */
	for (const auto &delayed_cond : *delayed_conditions) {
		auto it = get_item_by_name_mut(delayed_cond.sym, true);

		if (it == nullptr) {
			msg_err_cache (
					"cannot register delayed condition for %s",
					delayed_cond.sym.c_str());
			luaL_unref(delayed_cond.L, LUA_REGISTRYINDEX, delayed_cond.cbref);
		}
		else {
			if (!it->add_condition(delayed_cond.L, delayed_cond.cbref)) {
				msg_err_cache (
						"cannot register delayed condition for %s: virtual parent; qed",
						delayed_cond.sym.c_str());
				g_abort();
			}
		}
	}
	delayed_conditions.reset();

	for (auto &it : items_by_id) {
		it->process_deps(*this);
	}

	for (auto &it : virtual_symbols) {
		it->process_deps(*this);
	}

	/* Sorting stuff */
	auto postfilters_cmp = [](const auto &it1, const auto &it2) -> int {
		if (it1->priority > it2-> priority) {
			return 1;
		}
		else if (it1->priority == it2->priority) {
			return 0;
		}

		return -1;
	};
	auto prefilters_cmp = [](const auto &it1, const auto &it2) -> int {
		if (it1->priority > it2-> priority) {
			return -1;
		}
		else if (it1->priority == it2->priority) {
			return 0;
		}

		return 1;
	};


	std::stable_sort(std::begin(connfilters), std::end(connfilters), prefilters_cmp);
	std::stable_sort(std::begin(prefilters), std::end(prefilters), prefilters_cmp);
	std::stable_sort(std::begin(postfilters), std::end(postfilters), postfilters_cmp);
	std::stable_sort(std::begin(idempotent), std::end(idempotent), postfilters_cmp);

	rspamd_symcache_resort(cache);

	/* Connect metric symbols with symcache symbols */
	if (cfg->symbols) {
		g_hash_table_foreach(cfg->symbols,
				rspamd_symcache_metric_connect_cb,
				this);
	}

	return res;
}

auto symcache::load_items() -> bool
{
	auto cached_map = util::raii_mmaped_locked_file::mmap_shared(cfg->cache_filename,
			O_RDONLY, PROT_READ);

	if (!cached_map.has_value()) {
		msg_info_cache("%s", cached_map.error().c_str());
		return false;
	}


	if (cached_map->get_size() < (gint) sizeof(symcache_header)) {
		msg_info_cache("cannot use file %s, truncated: %z", cfg->cache_filename,
				errno, strerror(errno));
		return false;
	}

	const auto *hdr = (struct symcache_header *) cached_map->get_map();

	if (memcmp(hdr->magic, symcache_magic,
			sizeof(symcache_magic)) != 0) {
		msg_info_cache("cannot use file %s, bad magic", cfg->cache_filename);

		return false;
	}

	auto *parser = ucl_parser_new(0);
	const auto *p = (const std::uint8_t *) (hdr + 1);

	if (!ucl_parser_add_chunk(parser, p, cached_map->get_size() - sizeof(*hdr))) {
		msg_info_cache ("cannot use file %s, cannot parse: %s", cfg->cache_filename,
				ucl_parser_get_error(parser));
		ucl_parser_free(parser);

		return false;
	}

	auto *top = ucl_parser_get_object(parser);
	ucl_parser_free(parser);

	if (top == nullptr || ucl_object_type(top) != UCL_OBJECT) {
		msg_info_cache ("cannot use file %s, bad object", cfg->cache_filename);
		ucl_object_unref(top);

		return false;
	}

	auto it = ucl_object_iterate_new(top);
	const ucl_object_t *cur;
	while ((cur = ucl_object_iterate_safe(it, true)) != nullptr) {
		auto item_it = items_by_symbol.find(ucl_object_key(cur));

		if (item_it != items_by_symbol.end()) {
			auto item = item_it->second;
			/* Copy saved info */
			/*
			 * XXX: don't save or load weight, it should be obtained from the
			 * metric
			 */
#if 0
			elt = ucl_object_lookup (cur, "weight");

			if (elt) {
				w = ucl_object_todouble (elt);
				if (w != 0) {
					item->weight = w;
				}
			}
#endif
			const auto *elt = ucl_object_lookup(cur, "time");
			if (elt) {
				item->st->avg_time = ucl_object_todouble(elt);
			}

			elt = ucl_object_lookup(cur, "count");
			if (elt) {
				item->st->total_hits = ucl_object_toint(elt);
				item->last_count = item->st->total_hits;
			}

			elt = ucl_object_lookup(cur, "frequency");
			if (elt && ucl_object_type(elt) == UCL_OBJECT) {
				const ucl_object_t *freq_elt;

				freq_elt = ucl_object_lookup(elt, "avg");

				if (freq_elt) {
					item->st->avg_frequency = ucl_object_todouble(freq_elt);
				}
				freq_elt = ucl_object_lookup(elt, "stddev");

				if (freq_elt) {
					item->st->stddev_frequency = ucl_object_todouble(freq_elt);
				}
			}

			if (item->is_virtual() && !(item->type & SYMBOL_TYPE_GHOST)) {
				const auto &parent = item->get_parent(*this);

				if (parent) {
					if (parent->st->weight < item->st->weight) {
						parent->st->weight = item->st->weight;
					}
				}
				/*
				 * We maintain avg_time for virtual symbols equal to the
				 * parent item avg_time
				 */
				item->st->avg_time = parent->st->avg_time;
			}

			total_weight += fabs(item->st->weight);
			total_hits += item->st->total_hits;
		}
	}

	ucl_object_iterate_free(it);
	ucl_object_unref(top);

	return true;
}

template<typename T>
static constexpr auto round_to_hundreds(T x)
{
	return (::floor(x) * 100.0) / 100.0;
}

bool symcache::save_items() const
{
	auto file_sink = util::raii_file_sink::create(cfg->cache_filename,
			O_WRONLY | O_TRUNC, 00644);

	if (!file_sink.has_value()) {
		if (errno == EEXIST) {
			/* Some other process is already writing data, give up silently */
			return false;
		}

		msg_err_cache("%s", file_sink.error().c_str());

		return false;
	}

	struct symcache_header hdr;
	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.magic, symcache_magic, sizeof(symcache_magic));

	if (write(file_sink->get_fd(), &hdr, sizeof(hdr)) == -1) {
		msg_err_cache("cannot write to file %s, error %d, %s", cfg->cache_filename,
				errno, strerror(errno));

		return false;
	}

	auto *top = ucl_object_typed_new(UCL_OBJECT);

	for (const auto &it : items_by_symbol) {
		auto item = it.second;
		auto elt = ucl_object_typed_new(UCL_OBJECT);
		ucl_object_insert_key(elt,
				ucl_object_fromdouble(round_to_hundreds(item->st->weight)),
				"weight", 0, false);
		ucl_object_insert_key(elt,
				ucl_object_fromdouble(round_to_hundreds(item->st->time_counter.mean)),
				"time", 0, false);
		ucl_object_insert_key(elt, ucl_object_fromint(item->st->total_hits),
				"count", 0, false);

		auto *freq = ucl_object_typed_new(UCL_OBJECT);
		ucl_object_insert_key(freq,
				ucl_object_fromdouble(round_to_hundreds(item->st->frequency_counter.mean)),
				"avg", 0, false);
		ucl_object_insert_key(freq,
				ucl_object_fromdouble(round_to_hundreds(item->st->frequency_counter.stddev)),
				"stddev", 0, false);
		ucl_object_insert_key(elt, freq, "frequency", 0, false);

		ucl_object_insert_key(top, elt, it.first.data(), 0, true);
	}

	auto fp = fdopen(file_sink->get_fd(), "a");
	auto *efunc = ucl_object_emit_file_funcs(fp);
	auto ret = ucl_object_emit_full(top, UCL_EMIT_JSON_COMPACT, efunc, nullptr);
	ucl_object_emit_funcs_free(efunc);
	ucl_object_unref(top);
	fclose(fp);

	return ret;
}


auto symcache::get_item_by_id(int id, bool resolve_parent) const -> const cache_item *
{
	if (id < 0 || id >= items_by_id.size()) {
		msg_err_cache("internal error: requested item with id %d, when we have just %d items in the cache",
				id, (int)items_by_id.size());
		return nullptr;
	}

	auto &ret = items_by_id[id];

	if (!ret) {
		msg_err_cache("internal error: requested item with id %d but it is empty; qed",
				id);
		return nullptr;
	}

	if (resolve_parent && ret->is_virtual()) {
		return ret->get_parent(*this);
	}

	return ret.get();
}

auto symcache::get_item_by_name(std::string_view name, bool resolve_parent) const -> const cache_item *
{
	auto it = items_by_symbol.find(name);

	if (it == items_by_symbol.end()) {
		return nullptr;
	}

	if (resolve_parent && it->second->is_virtual()) {
		return it->second->get_parent(*this);
	}

	return it->second.get();
}

auto symcache::get_item_by_name_mut(std::string_view name, bool resolve_parent) const -> cache_item *
{
	auto it = items_by_symbol.find(name);

	if (it == items_by_symbol.end()) {
		return nullptr;
	}

	if (resolve_parent && it->second->is_virtual()) {
		return (cache_item *) it->second->get_parent(*this);
	}

	return it->second.get();
}

auto symcache::add_dependency(int id_from, std::string_view to, int virtual_id_from)-> void
{
	g_assert (id_from >= 0 && id_from < (gint)items_by_id.size());
	const auto &source = items_by_id[id_from];
	g_assert (source.get() != nullptr);

	source->deps.emplace_back(cache_item_ptr{nullptr},
			std::string(to),
			id_from,
			-1);


	if (virtual_id_from >= 0) {
		g_assert (virtual_id_from < (gint)virtual_symbols.size());
		/* We need that for settings id propagation */
		const auto &vsource = virtual_symbols[virtual_id_from];
		g_assert (vsource.get() != nullptr);
		vsource->deps.emplace_back(cache_item_ptr{nullptr},
				std::string(to),
				-1,
				virtual_id_from);
	}
}



auto cache_item::get_parent(const symcache &cache) const -> const cache_item *
{
	if (is_virtual()) {
		const auto &virtual_sp = std::get<virtual_item>(specific);

		return virtual_sp.get_parent(cache);
	}

	return nullptr;
}

auto cache_item::process_deps(const symcache &cache) -> void
{
	/* Allow logging macros to work */
	auto log_tag = [&](){ return cache.log_tag(); };

	for (auto &dep : deps) {
		msg_debug_cache ("process real dependency %s on %s", symbol.c_str(), dep.sym.c_str());
		auto *dit = cache.get_item_by_name_mut(dep.sym, true);

		if (dep.vid >= 0) {
			/* Case of the virtual symbol that depends on another (maybe virtual) symbol */
			const auto *vdit = cache.get_item_by_name(dep.sym, false);

			if (!vdit) {
				if (dit) {
					msg_err_cache("cannot add dependency from %s on %s: no dependency symbol registered",
							dep.sym.c_str(), dit->symbol.c_str());
				}
			}
			else {
				msg_debug_cache("process virtual dependency %s(%d) on %s(%d)", symbol.c_str(),
						dep.vid, vdit->symbol.c_str(), vdit->id);

				std::size_t nids = 0;

				/* Propagate ids */
				msg_debug_cache("check id propagation for dependency %s from %s",
						symbol.c_str(), dit->symbol.c_str());

				const auto *ids = dit->allowed_ids.get_ids(nids);

				if (nids > 0) {
					msg_debug_cache("propagate allowed ids from %s to %s",
							dit->symbol.c_str(), symbol.c_str());

					allowed_ids.set_ids(ids, nids, cache.get_pool());
				}

				ids = dit->forbidden_ids.get_ids(nids);

				if (nids > 0) {
					msg_debug_cache("propagate forbidden ids from %s to %s",
							dit->symbol.c_str(), symbol.c_str());

					forbidden_ids.set_ids(ids, nids, cache.get_pool());
				}
			}
		}

		if (dit != nullptr) {
			if (!dit->is_filter()) {
				/*
				 * Check sanity:
				 * - filters -> prefilter dependency is OK and always satisfied
				 * - postfilter -> (filter, prefilter) dep is ok
				 * - idempotent -> (any) dep is OK
				 *
				 * Otherwise, emit error
				 * However, even if everything is fine this dep is useless ¯\_(ツ)_/¯
				 */
				auto ok_dep = false;

				if (is_filter()) {
					if (dit->is_filter()) {
						ok_dep = true;
					}
					else if (dit->type & SYMBOL_TYPE_PREFILTER) {
						ok_dep = true;
					}
				}
				else if (type & SYMBOL_TYPE_POSTFILTER) {
					if (dit->type & SYMBOL_TYPE_PREFILTER) {
						ok_dep = true;
					}
				}
				else if (type & SYMBOL_TYPE_IDEMPOTENT) {
					if (dit->type & (SYMBOL_TYPE_PREFILTER|SYMBOL_TYPE_POSTFILTER)) {
						ok_dep = true;
					}
				}
				else if (type & SYMBOL_TYPE_PREFILTER) {
					if (priority < dit->priority) {
						/* Also OK */
						ok_dep = true;
					}
				}

				if (!ok_dep) {
					msg_err_cache ("cannot add dependency from %s on %s: invalid symbol types",
							dep.sym.c_str(), symbol.c_str());

					continue;
				}
			}
			else {
				if (dit->id == id) {
					msg_err_cache ("cannot add dependency on self: %s -> %s "
								   "(resolved to %s)",
							symbol.c_str(), dep.sym.c_str(), dit->symbol.c_str());
				} else {
					/* Create a reverse dep */
					dit->rdeps.emplace_back(getptr(), dep.sym, id, -1);
					dep.item = dit->getptr();
					dep.id = dit->id;

					msg_debug_cache ("add dependency from %d on %d", id,
							dit->id);
				}
			}
		}
		else if (dep.id >= 0) {
			msg_err_cache ("cannot find dependency on symbol %s for symbol %s",
					dep.sym.c_str(), symbol.c_str());

			continue;
		}
	}

	// Remove empty deps
	deps.erase(std::remove_if(std::begin(deps), std::end(deps),
			[](const auto &dep){ return !dep.item; }), std::end(deps));
}

auto virtual_item::get_parent(const symcache &cache) const -> const cache_item *
{
	if (parent) {
		return parent.get();
	}

	return cache.get_item_by_id(parent_id, false);
}

}