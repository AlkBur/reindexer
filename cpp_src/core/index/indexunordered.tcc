#include "indexunordered.h"
#include "tools/errors.h"
#include "tools/logger.h"

namespace reindexer {

template <typename T>
KeyRef IndexUnordered<T>::Upsert(const KeyRef &key, IdType id) {
	if (key.Type() == KeyValueEmpty) {
		this->empty_ids_.Unsorted().Add(id, IdSet::Auto);
		// Return invalid ref
		return KeyRef();
	}

	auto keyIt = find(key);
	if (keyIt == this->idx_map.end())
		keyIt = this->idx_map.insert({static_cast<typename T::key_type>(key), typename T::mapped_type()}).first;
	keyIt->second.Unsorted().Add(id, this->opts_.IsPK() ? IdSet::Ordered : IdSet::Auto);
	tracker_.markUpdated(idx_map, &*keyIt);

	if (this->KeyType() == KeyValueString && this->opts_.GetCollateMode() != CollateNone) {
		return IndexStore<typename T::key_type>::Upsert(key, id);
	}

	return KeyRef(keyIt->first);
}

template <typename T>
void IndexUnordered<T>::Delete(const KeyRef &key, IdType id) {
	int delcnt = 0;
	if (key.Type() == KeyValueEmpty) {
		delcnt = this->empty_ids_.Unsorted().Erase(id);
		assert(delcnt);
		return;
	}

	auto keyIt = find(key);
	assertf(keyIt != this->idx_map.end(), "Delete unexists key from index '%s' id=%d", this->name.c_str(), id);
	delcnt = keyIt->second.Unsorted().Erase(id);
	(void)delcnt;
	// TODO: we have to implement removal of composite indexes (doesn't work right now)
	assertf(this->opts_.IsArray() || delcnt, "Delete unexists id from index '%s' id=%d,key=%s", this->name.c_str(), id,
			KeyValue(key).toString().c_str());

	tracker_.markUpdated(idx_map, &*keyIt);

	if (this->KeyType() == KeyValueString && this->opts_.GetCollateMode() != CollateNone) {
		IndexStore<typename T::key_type>::Delete(key, id);
	}
}

// special implementation for string: avoid allocation string for *_map::find
// !!!! Not thread safe. Do not use this in Select
template <typename T>
template <typename U, typename std::enable_if<is_string_map_key<U>::value || is_string_unord_map_key<T>::value>::type *>
typename T::iterator IndexUnordered<T>::find(const KeyRef &key) {
	p_string skey(key);
	this->tmpKeyVal_->assign(skey.data(), skey.length());
	return this->idx_map.find(this->tmpKeyVal_);
}

template <typename T>
template <typename U, typename std::enable_if<!is_string_map_key<U>::value && !is_string_unord_map_key<T>::value>::type *>
typename T::iterator IndexUnordered<T>::find(const KeyRef &key) {
	return this->idx_map.find(static_cast<typename T::key_type>(key));
}

template <typename T>
void IndexUnordered<T>::tryIdsetCache(const KeyValues &keys, CondType condition, SortType sortId,
									  std::function<void(SelectKeyResult &)> selector, SelectKeyResult &res) {
	auto cached = cache_->Get(IdSetCacheKey{keys, condition, sortId});

	if (cached.key) {
		if (!cached.val.ids->size()) {
			selector(res);
			cache_->Put(*cached.key, res.mergeIdsets());
		} else
			res.push_back(cached.val.ids);
	} else
		selector(res);
}

template <typename T>
SelectKeyResults IndexUnordered<T>::SelectKey(const KeyValues &keys, CondType condition, SortType sortId, Index::ResultType res_type) {
	if (res_type == Index::ForceComparator) return IndexStore<typename T::key_type>::SelectKey(keys, condition, sortId, res_type);
	assertf(!tracker_.updated_.size() && !tracker_.completeUpdated_, "Internal error: select from non commited index %s\n",
			this->name.c_str());

	SelectKeyResult res;

	switch (condition) {
		case CondEmpty:
			res.push_back(this->empty_ids_.Sorted(sortId));
			break;
		case CondAny:
			// Get set of any keys
			res.reserve(this->idx_map.size());
			for (auto &keyIt : this->idx_map) res.push_back(keyIt.second.Sorted(sortId));
			break;
		// Get set of keys or single key
		case CondEq:
		case CondSet:
			if (condition == CondEq && keys.size() < 1)
				throw Error(errParams, "For condition reuqired at least 1 argument, but provided 0");
			if (keys.size() > 1000 && res_type != Index::ForceIdset) {
				return IndexStore<typename T::key_type>::SelectKey(keys, condition, sortId, res_type);
			} else {
				T *i_map = &this->idx_map;
				auto selector = [&keys, sortId, i_map](SelectKeyResult &res) {
					res.reserve(keys.size());
					for (auto key : keys) {
						auto keyIt = i_map->find(static_cast<typename T::key_type>(key));
						if (keyIt != i_map->end()) res.push_back(keyIt->second.Sorted(sortId));
					}
				};

				// Get from cache
				if (res_type != Index::ForceIdset && keys.size() > 1) {
					tryIdsetCache(keys, condition, sortId, selector, res);
				} else
					selector(res);
			}
			break;
		case CondAllSet: {
			// Get set of key, where all request keys are present
			SelectKeyResults rslts;
			for (auto key : keys) {
				SelectKeyResult res1;
				key.convert(this->KeyType());
				auto keyIt = this->idx_map.find(static_cast<typename T::key_type>(key));
				if (keyIt == this->idx_map.end()) {
					rslts.clear();
					rslts.push_back(res1);
					return rslts;
				}
				res1.push_back(keyIt->second.Sorted(sortId));
				rslts.push_back(res1);
			}
			return rslts;
		}

		case CondGe:
		case CondLe:
		case CondRange:
		case CondGt:
		case CondLt:
			return IndexStore<typename T::key_type>::SelectKey(keys, condition, sortId, res_type);
		default:
			throw Error(errQueryExec, "Unknown query on index '%s'", this->name.c_str());
	}

	return SelectKeyResults(res);
}  // namespace reindexer

template <typename T>
void IndexUnordered<T>::DumpKeys() {
	fprintf(stderr, "Dumping index: %s,keys=%d\n", this->name.c_str(), int(this->idx_map.size()));
	for (auto &k : this->idx_map) {
		string buf;
		buf += KeyValue(k.first).toString() + ":";
		if (!k.second.Unsorted().size()) {
			buf += "<no ids>";
		} else {
			buf += k.second.Unsorted().Dump();
		}
		fprintf(stderr, "%s\n", buf.c_str());
	}
}

template <typename T>
void IndexUnordered<T>::Commit(const CommitContext &ctx) {
	if (ctx.phases() & CommitContext::MakeIdsets) {
		// reset cache
		cache_.reset(new IdSetCache());

		logPrintf(LogTrace, "IndexUnordered::Commit (%s) %d uniq keys, %d empty, %s", this->name.c_str(), this->idx_map.size(),
				  this->empty_ids_.Unsorted().size(), tracker_.completeUpdated_ ? "complete" : "partial");

		this->empty_ids_.Unsorted().Commit(ctx);
		if (tracker_.completeUpdated_) {
			for (auto &keyIt : this->idx_map) keyIt.second.Unsorted().Commit(ctx);

			for (auto keyIt = this->idx_map.begin(); keyIt != this->idx_map.end();) {
				if (!keyIt->second.Unsorted().size())
					keyIt = this->idx_map.erase(keyIt);
				else
					keyIt++;
			}
		} else {
			tracker_.commitUpdated(idx_map, ctx);
		}
		tracker_.completeUpdated_ = false;
		tracker_.updated_.clear();
	}
}

template <typename T>
void IndexUnordered<T>::UpdateSortedIds(const UpdateSortedContext &ctx) {
	logPrintf(LogTrace, "IndexUnordered::UpdateSortedIds (%s) %d uniq keys, %d empty", this->name.c_str(), this->idx_map.size(),
			  this->empty_ids_.Unsorted().size());
	// For all keys in index
	for (auto &keyIt : this->idx_map) {
		keyIt.second.UpdateSortedIds(ctx);
	}

	this->empty_ids_.UpdateSortedIds(ctx);
}

template <typename T>
Index *IndexUnordered<T>::Clone() {
	return new IndexUnordered<T>(*this);
}

}  // namespace reindexer
