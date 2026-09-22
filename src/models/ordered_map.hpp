#pragma once

// 保持插入顺序的小 map。
//
// 为什么不用 std::map：Python 的 dict 保持插入顺序，而资产库的
// characters / locations 在接口响应里是**数组**——数组顺序是值的一部分，
// 前端就按那个顺序渲染列表。用 std::map 会按 key 排序，
// 两个后端返回的场景列表顺序不同，这是肉眼可见的差异。
//
// 但同一份数据还有另一个需求：character_ids() 要返回**排序后**的列表
// （Python 那边是 sorted(self.characters)，给大模型做约束解码用）。
// 所以顺序和排序是两件事，分别由迭代和 sorted_keys() 提供。
//
// 还有一层坑：nlohmann::json 内部就是 std::map，parse 一个 JSON object
// 时文档顺序当场就丢了。所以读资产库必须用 nlohmann::ordered_json，
// 见 ProjectStore::load_assets。
//
// 容器本身用 vector 线性查找。资产库量级是个位数到几十，
// 哈希或红黑树的收益不存在，而顺序语义要自己维护反而是负担。

#include <algorithm>

#include "util/say.hpp"
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::models {

template <typename V>
class OrderedMap {
public:
    using value_type = std::pair<std::string, V>;
    using const_iterator = typename std::vector<value_type>::const_iterator;
    using iterator = typename std::vector<value_type>::iterator;

    bool empty() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }
    void clear() { items_.clear(); }

    const_iterator begin() const { return items_.begin(); }
    const_iterator end() const { return items_.end(); }
    iterator begin() { return items_.begin(); }
    iterator end() { return items_.end(); }

    const_iterator find(const std::string& key) const {
        return std::find_if(items_.begin(), items_.end(),
                            [&](const value_type& kv) { return kv.first == key; });
    }
    iterator find(const std::string& key) {
        return std::find_if(items_.begin(), items_.end(),
                            [&](value_type& kv) { return kv.first == key; });
    }

    std::size_t count(const std::string& key) const {
        return find(key) == end() ? 0u : 1u;
    }

    bool contains(const std::string& key) const { return count(key) != 0; }

    const V& at(const std::string& key) const {
        const auto it = find(key);
        if (it == items_.end()) throw std::out_of_range(SAYF("没有这个键：%1", key));
        return it->second;
    }
    V& at(const std::string& key) {
        const auto it = find(key);
        if (it == items_.end()) throw std::out_of_range(SAYF("没有这个键：%1", key));
        return it->second;
    }

    /// 已有就覆盖值、保持原位置；没有就追加到末尾。
    /// 这与 Python 的 dict[k] = v 语义一致——重新赋值不会把键挪到末尾。
    V& operator[](const std::string& key) {
        const auto it = find(key);
        if (it != items_.end()) return it->second;
        items_.emplace_back(key, V{});
        return items_.back().second;
    }

    void erase(const std::string& key) {
        const auto it = find(key);
        if (it != items_.end()) items_.erase(it);
    }

    /// 排序后的键。对应 Python 的 sorted(d)。
    std::vector<std::string> sorted_keys() const {
        std::vector<std::string> out;
        out.reserve(items_.size());
        for (const auto& kv : items_) out.push_back(kv.first);
        std::sort(out.begin(), out.end());
        return out;
    }

    bool operator==(const OrderedMap& o) const { return items_ == o.items_; }
    bool operator!=(const OrderedMap& o) const { return !(*this == o); }

private:
    std::vector<value_type> items_;
};

// 序列化成 JSON object。模板参数留成 BasicJsonType，
// 这样 json 和 ordered_json 都能用——读资产库时必须用后者才不丢顺序。
template <typename BasicJsonType, typename V>
void to_json(BasicJsonType& j, const OrderedMap<V>& m) {
    j = BasicJsonType::object();
    // 值经由普通 nlohmann::json 中转。
    //
    // 直接写 j[kv.first] = kv.second 在 J 是 ordered_json 时编不过：
    // 各个模型结构体的序列化是 NLOHMANN_DEFINE_TYPE_INTRUSIVE 生成的，
    // 那个宏的形参写死是 nlohmann::json&，不是模板。
    // 走一次 basic_json 之间的转换构造就绕开了。
    //
    // 结构体内部的字段顺序会因此变成字典序，但那不属于契约——
    // 方案第六节明写数据层对拍是"字段顺序无关的深比较"。
    // 真正要保的是**这里的键顺序**，因为它决定接口响应里数组的顺序。
    for (const auto& kv : m) j[kv.first] = BasicJsonType(nlohmann::json(kv.second));
}

template <typename BasicJsonType, typename V>
void from_json(const BasicJsonType& j, OrderedMap<V>& m) {
    m.clear();
    if (!j.is_object()) return;
    // items() 按底层容器的顺序走。传进来的是 ordered_json 时那就是文档顺序，
    // 是普通 json 时已经被排过序了——所以调用方要负责用对类型。
    for (auto it = j.begin(); it != j.end(); ++it) {
        m[it.key()] = nlohmann::json(it.value()).template get<V>();
    }
}

}  // namespace changji::models
