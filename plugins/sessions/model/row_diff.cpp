#include "sessions/model/row_diff.hpp"

#include <QHash>
#include <QSet>
#include <QString>

#include <algorithm>

namespace roost::sessions {

std::vector<RowStep> DiffRows(const std::vector<Row>& before,
                              const std::vector<Row>& after) {
    std::vector<RowStep> steps;
    QSet<QString> afterKeys;
    for (const Row& row : after) {
        afterKeys.insert(row.Key);
    }

    std::vector<QString> keys;
    for (const Row& row : before) {
        keys.push_back(row.Key);
    }
    for (int i = static_cast<int>(keys.size()) - 1; i >= 0; --i) {
        if (!afterKeys.contains(keys[i])) {
            steps.push_back({ERowStep::Remove, i, -1});
            keys.erase(keys.begin() + i);
        }
    }

    const int wanted = static_cast<int>(after.size());
    for (int i = 0; i < wanted; ++i) {
        if (i < static_cast<int>(keys.size()) && keys[i] == after[i].Key) {
            continue;
        }
        const auto found =
            std::find(keys.begin() + std::min<std::ptrdiff_t>(i, keys.size()),
                      keys.end(), after[i].Key);
        if (found != keys.end()) {
            const int from = static_cast<int>(found - keys.begin());
            steps.push_back({ERowStep::Move, from, i});
            keys.erase(found);
            keys.insert(keys.begin() + i, after[i].Key);
        } else {
            steps.push_back({ERowStep::Insert, -1, i});
            keys.insert(keys.begin() + i, after[i].Key);
        }
    }
    // Rows of a repeated key that `after` does not have as often.
    for (int i = static_cast<int>(keys.size()) - 1; i >= wanted; --i) {
        steps.push_back({ERowStep::Remove, i, -1});
        keys.pop_back();
    }

    QHash<QString, const Row*> kept;
    for (const Row& row : before) {
        kept.insert(row.Key, &row);
    }
    for (int i = 0; i < wanted; ++i) {
        const auto old = kept.constFind(after[i].Key);
        if (old != kept.cend() && !(**old == after[i])) {
            steps.push_back({ERowStep::Change, -1, i});
        }
    }
    return steps;
}

}  // namespace roost::sessions
