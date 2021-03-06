// Copyright 2019 DeepMind Technologies Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "open_spiel/policy.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "open_spiel/abseil-cpp/absl/algorithm/container.h"
#include "open_spiel/abseil-cpp/absl/container/flat_hash_set.h"
#include "open_spiel/abseil-cpp/absl/strings/charconv.h"
#include "open_spiel/abseil-cpp/absl/strings/str_cat.h"
#include "open_spiel/abseil-cpp/absl/strings/str_format.h"
#include "open_spiel/abseil-cpp/absl/types/optional.h"
#include "open_spiel/spiel.h"
#include "open_spiel/spiel_utils.h"

namespace open_spiel {

void SetProb(ActionsAndProbs* actions_and_probs, Action action, double prob) {
  for (auto& iter : *actions_and_probs) {
    if (iter.first == action) {
      iter.second = prob;
      return;
    }
  }
  actions_and_probs->push_back({action, prob});
}

double GetProb(const ActionsAndProbs& action_and_probs, Action action) {
  auto it = absl::c_find_if(action_and_probs,
                            [&action](const std::pair<Action, double>& p) {
                              return p.first == action;
                            });
  if (it == action_and_probs.end()) return -1.;
  return it->second;
}

Action GetAction(const ActionsAndProbs& action_and_probs) {
  for (const auto& iter : action_and_probs) {
    if (iter.second == 1.0) {
      return iter.first;
    }
  }
  return kInvalidAction;
}

ActionsAndProbs ToDeterministicPolicy(const ActionsAndProbs& actions_and_probs,
                                      Action action) {
  ActionsAndProbs new_policy;
  new_policy.reserve(actions_and_probs.size());
  for (const auto& iter : actions_and_probs) {
    new_policy.push_back({iter.first, iter.first == action ? 1.0 : 0.0});
  }
  return new_policy;
}

ActionsAndProbs UniformStatePolicy(const State& state) {
  ActionsAndProbs actions_and_probs;
  std::vector<Action> actions = state.LegalActions();
  actions_and_probs.reserve(actions.size());
  absl::c_for_each(actions, [&actions_and_probs, &actions](Action a) {
    actions_and_probs.push_back({a, 1. / static_cast<double>(actions.size())});
  });
  return actions_and_probs;
}

ActionsAndProbs FirstActionStatePolicy(const State& state) {
  return {{state.LegalActions()[0], 1.0}};
}

std::unique_ptr<Policy> DeserializePolicy(const std::string& serialized,
                                          std::string delimiter) {
  // Class’s identity is the very first line, see Policy::Serialize
  // for more info.
  std::pair<std::string, absl::string_view> cls_and_content =
      absl::StrSplit(serialized, absl::MaxSplits(':', 1));
  std::string class_identity = cls_and_content.first;

  if (class_identity == "TabularPolicy") {
    return DeserializeTabularPolicy(serialized, delimiter);
  } else if (class_identity == "UniformPolicy") {
    return std::make_unique<UniformPolicy>();
  } else {
    SpielFatalError(absl::StrCat("Deserialization of ", class_identity,
                                 " is not supported."));
  }
}

TabularPolicy::TabularPolicy(const Game& game)
    : TabularPolicy(GetRandomPolicy(game)) {}

std::unique_ptr<TabularPolicy> DeserializeTabularPolicy(
    const std::string& serialized, std::string delimiter) {
  // Class’s identity is the very first line, see Policy::Serialize
  // for more info.
  std::pair<std::string, absl::string_view> cls_and_content =
      absl::StrSplit(serialized, absl::MaxSplits(':', 1));
  SPIEL_CHECK_EQ(cls_and_content.first, "TabularPolicy");

  std::unique_ptr<TabularPolicy> res = std::make_unique<TabularPolicy>();
  if (cls_and_content.second.empty()) return res;

  std::vector<absl::string_view> splits =
      absl::StrSplit(cls_and_content.second, delimiter);

  // Insert the actual values
  Action action;
  double prob;
  for (int i = 0; i < splits.size(); i += 2) {
    std::vector<absl::string_view> policy_values =
        absl::StrSplit(splits.at(i + 1), ',');
    ActionsAndProbs res_policy;
    res_policy.reserve(policy_values.size());

    for (absl::string_view policy_value : policy_values) {
      std::pair<absl::string_view, absl::string_view> action_and_prob =
          absl::StrSplit(policy_value, '=');
      SPIEL_CHECK_TRUE(absl::SimpleAtoi(action_and_prob.first, &action));
      absl::from_chars(
          action_and_prob.second.data(),
          action_and_prob.second.data() + action_and_prob.second.size(), prob);
      res_policy.push_back({action, prob});
    }
    res->SetStatePolicy(std::string(splits.at(i)), res_policy);
  }
  return res;
}

const std::string TabularPolicy::ToString() const {
  std::string str = "";
  for (const auto& infostate_and_policy : policy_table_) {
    absl::StrAppend(&str, infostate_and_policy.first, ": ");
    for (const auto& policy : infostate_and_policy.second) {
      absl::StrAppend(&str, " ", policy.first, "=", policy.second);
    }
    absl::StrAppend(&str, "\n");
  }
  return str;
}

const std::string TabularPolicy::ToStringSorted() const {
  std::vector<std::string> keys;
  keys.reserve(policy_table_.size());

  for (const auto& infostate_and_policy : policy_table_) {
    keys.push_back(infostate_and_policy.first);
  }

  std::sort(keys.begin(), keys.end());
  std::string str = "";
  for (const std::string& key : keys) {
    absl::StrAppend(&str, key, ": ");
    for (const auto& policy : policy_table_.at(key)) {
      absl::StrAppend(&str, " ", policy.first, "=", policy.second);
    }
    absl::StrAppend(&str, "\n");
  }

  return str;
}

TabularPolicy GetEmptyTabularPolicy(const Game& game,
                                    bool initialize_to_uniform) {
  std::unordered_map<std::string, ActionsAndProbs> policy;
  if (game.GetType().dynamics != GameType::Dynamics::kSequential) {
    SpielFatalError("Game is not sequential.");
    return TabularPolicy(policy);
  }
  std::list<std::unique_ptr<State>> to_visit;
  to_visit.push_back(game.NewInitialState());
  while (!to_visit.empty()) {
    std::unique_ptr<State> state = std::move(to_visit.back());
    to_visit.pop_back();
    if (state->IsTerminal()) {
      continue;
    }
    if (state->IsChanceNode()) {
      for (const auto& outcome_and_prob : state->ChanceOutcomes()) {
        to_visit.emplace_back(state->Child(outcome_and_prob.first));
      }
    } else {
      ActionsAndProbs infostate_policy;
      std::vector<Action> legal_actions = state->LegalActions();
      const int num_legal_actions = legal_actions.size();
      SPIEL_CHECK_GT(num_legal_actions, 0.);
      double action_probability = 1.;
      if (initialize_to_uniform) {
        action_probability = 1. / num_legal_actions;
      }

      infostate_policy.reserve(num_legal_actions);
      for (Action action : legal_actions) {
        to_visit.push_back(state->Child(action));
        infostate_policy.push_back({action, action_probability});
      }
      if (infostate_policy.empty()) {
        SpielFatalError("State has zero legal actions.");
      }
      policy.insert({state->InformationStateString(), infostate_policy});
    }
  }
  return TabularPolicy(policy);
}

TabularPolicy GetUniformPolicy(const Game& game) {
  return GetEmptyTabularPolicy(game, /*initialize_to_uniform=*/true);
}

TabularPolicy GetRandomPolicy(const Game& game, int seed) {
  std::mt19937 gen(seed);
  std::uniform_real_distribution<double> dist(0, 1);
  TabularPolicy policy = GetEmptyTabularPolicy(game);
  std::unordered_map<std::string, ActionsAndProbs>& policy_table =
      policy.PolicyTable();
  for (auto& kv : policy_table) {
    ActionsAndProbs state_policy;
    if (kv.second.empty()) {
      SpielFatalError("State has zero legal actions.");
    }
    state_policy.reserve(kv.second.size());
    double sum = 0;
    double prob;
    for (const auto& action_and_prob : kv.second) {
      // We multiply the original probability by a random number between 0
      // and 1. We then normalize. This has the effect of randomly permuting the
      // policy but all illegal actions still have zero probability.
      prob = dist(gen) * action_and_prob.second;
      sum += prob;
      state_policy.push_back({action_and_prob.first, prob});
    }
    // We normalize the policy to ensure it sums to 1.
    for (auto& action_and_prob : state_policy) {
      action_and_prob.second /= sum;
    }
    // This is included as a sanity check.
    double normalized_sum = 0;
    for (auto& action_and_prob : state_policy) {
      normalized_sum += action_and_prob.second;
    }
    SPIEL_CHECK_FLOAT_EQ(normalized_sum, 1.0);
    kv.second = state_policy;
  }
  return policy;
}

TabularPolicy GetFirstActionPolicy(const Game& game) {
  std::unordered_map<std::string, ActionsAndProbs> policy;
  if (game.GetType().dynamics != GameType::Dynamics::kSequential) {
    SpielFatalError("Game is not sequential.");
    return TabularPolicy(policy);
  }
  std::vector<std::unique_ptr<State>> to_visit;
  to_visit.push_back(game.NewInitialState());
  while (!to_visit.empty()) {
    std::unique_ptr<State> state = std::move(to_visit.back());
    to_visit.pop_back();
    if (state->IsTerminal()) {
      continue;
    }
    if (state->IsChanceNode()) {
      for (const auto& outcome_and_prob : state->ChanceOutcomes()) {
        to_visit.emplace_back(state->Child(outcome_and_prob.first));
      }
    } else {
      ActionsAndProbs infostate_policy;
      std::vector<Action> legal_actions = state->LegalActions();
      const int num_legal_actions = legal_actions.size();
      SPIEL_CHECK_GT(num_legal_actions, 0.);
      bool first_legal_action_found = false;

      infostate_policy.reserve(num_legal_actions);
      for (Action action : legal_actions) {
        to_visit.push_back(state->Child(action));
        if (!first_legal_action_found) {
          first_legal_action_found = true;
          infostate_policy.push_back({action, 1.});

        } else {
          infostate_policy.push_back({action, 0.});
        }
      }
      if (infostate_policy.empty()) {
        SpielFatalError("State has zero legal actions.");
      }
      policy[state->InformationStateString()] = std::move(infostate_policy);
    }
  }
  return TabularPolicy(policy);
}

TabularPolicy GetPrefActionPolicy(
    const Game& game, const std::vector<Action>& pref_actions) {
  std::unordered_map<std::string, ActionsAndProbs> policy;
  if (game.GetType().dynamics != GameType::Dynamics::kSequential) {
    SpielFatalError("Game is not sequential.");
  }
  // Consider using GetAllHistories here instead of custom DFS to ensure that
  // all edge cases are properly handled. Please see version control for a full
  // discussion.
  std::vector<std::unique_ptr<State>> to_visit;
  to_visit.push_back(game.NewInitialState());
  while (!to_visit.empty()) {
    std::unique_ptr<State> state = std::move(to_visit.back());
    to_visit.pop_back();
    if (state->IsTerminal()) {
      continue;
    }
    if (state->IsChanceNode()) {
      for (const auto& [outcome, _] : state->ChanceOutcomes()) {
        to_visit.emplace_back(state->Child(outcome));
      }
    } else {
      std::vector<Action> legal_actions = state->LegalActions();
      const int num_legal_actions = legal_actions.size();
      SPIEL_CHECK_FALSE(legal_actions.empty());

      absl::optional<Action> legal_pref_action;
      absl::flat_hash_set<Action> pref_actions_set(
          pref_actions.begin(), pref_actions.end());
      for (Action legal_action : legal_actions) {
        if (pref_actions_set.contains(legal_action)) {
          legal_pref_action = legal_action;
          break;
        }
      }
      if (!legal_pref_action) SpielFatalError("Legal pref action not found.");

      ActionsAndProbs infostate_policy;
      infostate_policy.reserve(num_legal_actions);
      for (Action action : legal_actions) {
        to_visit.push_back(state->Child(action));
        if (action == legal_pref_action) {
          infostate_policy.push_back({action, 1.});
        } else {
          infostate_policy.push_back({action, 0.});
        }
      }
      if (infostate_policy.empty()) {
        SpielFatalError("State has zero legal actions.");
      }
      policy[state->InformationStateString()] = std::move(infostate_policy);
    }
  }
  return TabularPolicy(policy);
}


std::string PrintPolicy(const ActionsAndProbs& policy) {
  std::string policy_string;
  for (auto [a, p] : policy) {
    absl::StrAppend(&policy_string, absl::StrFormat("(%i, %f), ", a, p));
  }
  return policy_string;
}

}  // namespace open_spiel
