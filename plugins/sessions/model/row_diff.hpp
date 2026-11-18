#pragma once

#include <vector>

#include "sessions/model/session_rows.hpp"

// How the rows of the list change from one build to the next, as the steps a
// Qt list model announces: never a reset, so a view keeps its position and
// its delegates through a state change of one session.
namespace roost::sessions {

enum class ERowStep { Remove, Move, Insert, Change };

struct RowStep {
    ERowStep Kind = ERowStep::Change;
    // Remove, Move: the row before the step.
    int From = -1;
    // Move: the row it takes; Insert, Change: the row it concerns. Rows after
    // all the earlier steps were applied.
    int To = -1;

    bool operator==(const RowStep&) const = default;
};

// The steps that turn `before` into `after`, to apply one after the other:
// removals (last row first), then the moves and inserts that put each row
// into its place from the top, then a Change for each row that kept its key
// but not its content. A Move takes the row From to To, where To < From, and
// shifts the rows between by one. Rows are told apart by Key, which is
// unique in the rows BuildRows gives.
std::vector<RowStep> DiffRows(const std::vector<Row>& before,
                              const std::vector<Row>& after);

}  // namespace roost::sessions
