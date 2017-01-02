// Copyright (c) 2016, Coveo Solutions Inc.
// Distributed under the MIT license (see LICENSE).

// Implementation details of coveo::linq operators.

#ifndef COVEO_LINQ_DETAIL_H
#define COVEO_LINQ_DETAIL_H

#include <coveo/enumerable/detail/enumerable_detail.h>
#include <coveo/linq/exception.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <deque>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <type_traits>
#include <vector>
#include <utility>

namespace coveo {
namespace linq {
namespace detail {

// Traits class used by LINQ operators. A shorthand for enumerable's seq_element_traits
// that infers the sequence's value_type from the return value of its iterators.
// Also provides the type of iterator used by the sequence.
template<typename Seq>
struct seq_traits : public coveo::detail::seq_element_traits<decltype(*std::begin(std::declval<Seq>()))>
{
    typedef std::decay_t<decltype(std::begin(std::declval<Seq>()))>     iterator_type;  // Type of iterator used by the sequence
};
template<typename Seq> struct seq_traits<Seq&> : seq_traits<Seq> { };
template<typename Seq> struct seq_traits<Seq&&> : seq_traits<Seq> { };
template<typename Seq> struct seq_traits<std::reference_wrapper<Seq>> : seq_traits<Seq> { };

// Proxy comparator that references an external predicate.
// Allows instances of lambdas to be used as predicates for sets, for instance.
template<typename Pred>
class proxy_cmp
{
private:
    const std::decay_t<Pred>* ppred_;

public:
    explicit proxy_cmp(const Pred& pred)
        : ppred_(std::addressof(pred)) { }
    
    template<typename T, typename U>
    auto operator()(const T& left, const U& right) const
        -> decltype((*ppred_)(left, right))
    {
        return (*ppred_)(left, right);
    }
};

// Selector implementation that can be used with some operators
// that return an element and its index, when the index is not needed.
template<typename Selector>
class indexless_selector_proxy
{
private:
    Selector sel_;  // Selector that doesn't care about index

public:
    explicit indexless_selector_proxy(Selector&& sel)
        : sel_(std::forward<Selector>(sel)) { }

    template<typename T>
    decltype(auto) operator()(T&& element, std::size_t) {
        return sel_(element);
    }
};

// Transparent predicate that returns values unmodified.
template<typename = void>
struct identity {
    template<typename T>
    auto operator()(T&& obj) const {
        return std::forward<T>(obj);
    }
};

// Transparent binary predicate that returns a pair
// of its two arguments.
template<typename = void>
struct pair_of {
    template<typename T, typename U>
    auto operator()(T&& obj1, U&& obj2) const {
        return std::pair<T, U>(std::forward<T>(obj1), std::forward<U>(obj2));
    }
};

// Utility methods to throw LINQ-specific exceptions.
template<typename = void>
[[noreturn]] void throw_linq_empty_sequence() {
    throw empty_sequence("empty_sequence");
}
template<typename = void>
[[noreturn]] void throw_linq_out_of_range() {
    throw out_of_range("out_of_range");
}

// Implementation of concat operator.
template<typename Seq2>
class concat_impl
{
public:
    // Implementation of next delegate that concatenates two sequences
    template<typename Seq1>
    class next_impl
    {
    private:
        // Type of iterators for both sequences
        typedef typename seq_traits<Seq1>::iterator_type first_iterator_type;
        typedef typename seq_traits<Seq2>::iterator_type second_iterator_type;

        // Information used to concatenate sequences. Shared among delegates.
        class concat_info
        {
        private:
            Seq1 seq1_;                     // First sequence to concatenate
            first_iterator_type iend1_;     // End of first sequence
            Seq2 seq2_;                     // Second sequence to concatenate
            second_iterator_type iend2_;    // End of second sequence

        public:
            concat_info(Seq1&& seq1, Seq2&& seq2)
                : seq1_(std::forward<Seq1>(seq1)),
                  iend1_(std::end(seq1_)),
                  seq2_(std::forward<Seq2>(seq2)),
                  iend2_(std::end(seq2_)) { }

            // Cannot move/copy, stored in a shared_ptr
            concat_info(const concat_info&) = delete;
            concat_info& operator=(const concat_info&) = delete;

            first_iterator_type first_begin() {
                return std::begin(seq1_);
            }
            second_iterator_type second_begin() {
                return std::begin(seq2_);
            }

            // Returns next element from one of the sequences or nullptr when done
            auto get_next(first_iterator_type& icur1, second_iterator_type& icur2)
                -> typename seq_traits<Seq1>::const_pointer
            {
                // First return all elements from first sequence, then from second sequence.
                typename seq_traits<Seq1>::const_pointer pobj = nullptr;
                if (icur1 != iend1_) {
                    typename seq_traits<Seq1>::const_reference robj = *icur1;
                    pobj = std::addressof(robj);
                    ++icur1;
                } else if (icur2 != iend2_) {
                    typename seq_traits<Seq2>::const_reference robj = *icur2;
                    pobj = std::addressof(robj);
                    ++icur2;
                }
                return pobj;
            }
        };
        typedef std::shared_ptr<concat_info> concat_info_sp;

        concat_info_sp spinfo_;         // Shared concat info
        first_iterator_type icur1_;     // Current position in first sequence
        second_iterator_type icur2_;    // Current position in second sequence

    public:
        next_impl(Seq1&& seq1, Seq2&& seq2)
            : spinfo_(std::make_shared<concat_info>(std::forward<Seq1>(seq1), std::forward<Seq2>(seq2))),
              icur1_(spinfo_->first_begin()), icur2_(spinfo_->second_begin()) { }

        template<typename Op>
        auto operator()(Op&) {
            return spinfo_->get_next(icur1_, icur2_);
        }
    };

private:
    Seq2 seq2_;     // Second sequence (possibly a ref)
#ifdef _DEBUG
    bool applied_;  // Tracks operator application
#endif

public:
    explicit concat_impl(Seq2&& seq2)
        : seq2_(std::forward<Seq2>(seq2))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    // Movable but not copyable
    concat_impl(const concat_impl&) = delete;
    concat_impl(concat_impl&&) = default;
    concat_impl& operator=(const concat_impl&) = delete;
    concat_impl& operator=(concat_impl&&) = default;

    template<typename Seq1>
    auto operator()(Seq1&& seq1)
        -> coveo::enumerable<typename seq_traits<Seq1>::raw_value_type>
    {
        // Note: if seq2_ is not a ref, it is moved by the forward below
        // and becomes invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        return next_impl<Seq1>(std::forward<Seq1>(seq1), std::forward<Seq2>(seq2_));
    }
};

// Implementation of distinct operator.
template<typename Pred>
class distinct_impl
{
public:
    // Implementation of next delegate that filters duplicate elements
    template<typename Seq>
    class next_impl
    {
    private:
        // Type of iterator for the sequence
        typedef typename seq_traits<Seq>::iterator_type iterator_type;

        // Set storing seen elements
        typedef std::set<typename seq_traits<Seq>::raw_value_type, proxy_cmp<Pred>>
                                                        seen_elements_set;

        // Info used to produce distinct elements. Shared among delegates.
        class distinct_info
        {
        private:
            Seq seq_;               // Sequence being iterated
            iterator_type iend_;    // Iterator pointing at end of sequence
            Pred pred_;             // Predicate ordering the elements

        public:
            distinct_info(Seq&& seq, Pred&& pred)
                : seq_(std::forward<Seq>(seq)),
                  iend_(std::end(seq_)),
                  pred_(std::forward<Pred>(pred)) { }

            // Cannot copy/move, stored in a shared_ptr
            distinct_info(const distinct_info&) = delete;
            distinct_info& operator=(const distinct_info&) = delete;

            iterator_type seq_begin() {
                return std::begin(seq_);
            }
            seen_elements_set init_seen_elements() {
                return seen_elements_set(proxy_cmp<Pred>(pred_));
            }

            // Returns next distinct element or nullptr when done
            auto get_next(iterator_type& icur, seen_elements_set& seen)
                -> typename seq_traits<Seq>::const_pointer
            {
                typename seq_traits<Seq>::const_pointer pobj = nullptr;
                for (; pobj == nullptr && icur != iend_; ++icur) {
                    typename seq_traits<Seq>::const_reference robj = *icur;
                    if (seen.emplace(robj).second) {
                        // Not seen yet, return this element.
                        pobj = std::addressof(robj);
                    }
                }
                return pobj;
            }
        };
        typedef std::shared_ptr<distinct_info> distinct_info_sp;

        distinct_info_sp spinfo_;   // Shared info
        iterator_type icur_;        // Iterator pointing at current element
        seen_elements_set seen_;    // Set of seen elements

    public:
        next_impl(Seq&& seq, Pred&& pred)
            : spinfo_(std::make_shared<distinct_info>(std::forward<Seq>(seq), std::forward<Pred>(pred))),
              icur_(spinfo_->seq_begin()), seen_(spinfo_->init_seen_elements()) { }

        template<typename Op>
        auto operator()(Op&) {
            return spinfo_->get_next(icur_, seen_);
        }
    };

private:
    Pred pred_;     // Predicate used to compare elements
#ifdef _DEBUG
    bool applied_;  // Tracks operator application
#endif

public:
    explicit distinct_impl(Pred&& pred)
        : pred_(std::forward<Pred>(pred))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    // Movable but not copyable
    distinct_impl(const distinct_impl&) = delete;
    distinct_impl(distinct_impl&&) = default;
    distinct_impl& operator=(const distinct_impl&) = delete;
    distinct_impl& operator=(distinct_impl&&) = default;

    template<typename Seq>
    auto operator()(Seq&& seq)
        -> coveo::enumerable<typename seq_traits<Seq>::raw_value_type>
    {
        // Note: if pred_ is not a ref, it is moved by the forward below
        // and becomes invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        // #clp TODO optimize if sequence does not have duplicates already, like std::set?
        return next_impl<Seq>(std::forward<Seq>(seq), std::forward<Pred>(pred_));
    }
};

// Implementation of except operator.
template<typename Seq2, typename Pred>
class except_impl
{
public:
    // Implementation of next delegate that filters out
    // elements in the second sequence
    template<typename Seq1>
    class next_impl
    {
    private:
        // Vector of elements in the second sequence. Used to filter them out.
        typedef std::vector<typename seq_traits<Seq2>::raw_value_type> elements_to_filter_v;

        // Type of iterator for the first sequence.
        typedef typename seq_traits<Seq1>::iterator_type first_iterator_type;

        // Bean storing info about elements to filter out. Shared among delegate instances.
        class filter_info
        {
        private:
            Seq1 seq1_;                         // Sequence of elements to scan and return.
            first_iterator_type iend1_;         // End of seq1_
            Seq2 seq2_;                         // Sequence of elements to filter out
            Pred pred_;                         // Predicate used to compare
            elements_to_filter_v v_to_filter_;  // Elements to filter out. Late-initialized.
            bool init_;                         // Init flag for v_to_filter_.

        public:
            filter_info(Seq1&& seq1, Seq2&& seq2, Pred&& pred)
                : seq1_(std::forward<Seq1>(seq1)), iend1_(std::end(seq1_)),
                  seq2_(std::forward<Seq2>(seq2)), pred_(std::forward<Pred>(pred)),
                  v_to_filter_(), init_(false) { }

            // No move or copy, it's stored in a shared_ptr
            filter_info(const filter_info&) = delete;
            filter_info& operator=(const filter_info&) = delete;

            first_iterator_type first_begin() {
                return std::begin(seq1_);
            }

            bool filtered(typename seq_traits<Seq1>::const_reference obj) {
                if (!init_) {
                    // Init elements to filter on first call
                    v_to_filter_.insert(v_to_filter_.end(), std::begin(seq2_), std::end(seq2_));
                    std::sort(v_to_filter_.begin(), v_to_filter_.end(), pred_);
                    init_ = true;
                }
                return std::binary_search(v_to_filter_.cbegin(), v_to_filter_.cend(), obj, pred_);
            }

            // Returns next non-filtered element or nullptr when done
            auto get_next(first_iterator_type& icur1)
                -> typename seq_traits<Seq1>::const_pointer
            {
                typename seq_traits<Seq1>::const_pointer pobj = nullptr;
                for (; pobj == nullptr && icur1 != iend1_; ++icur1) {
                    typename seq_traits<Seq1>::const_reference robj = *icur1;
                    if (!filtered(robj)) {
                        pobj = std::addressof(robj);
                    }
                }
                return pobj;
            }
        };
        typedef std::shared_ptr<filter_info> filter_info_sp;

        filter_info_sp spfilter_;   // Bean containing filter info
        first_iterator_type icur_;  // Current position in first sequence

    public:
        next_impl(Seq1&& seq1, Seq2&& seq2, Pred&& pred)
            : spfilter_(std::make_shared<filter_info>(std::forward<Seq1>(seq1),
                                                      std::forward<Seq2>(seq2),
                                                      std::forward<Pred>(pred))),
              icur_(spfilter_->first_begin()) { }

        template<typename Op>
        auto operator()(Op&) {
            return spfilter_->get_next(icur_);
        }
    };

private:
    Seq2 seq2_;     // Sequence of elements to filter out
    Pred pred_;     // Predicate used to compare elements
#ifdef _DEBUG
    bool applied_;  // Tracks operator application
#endif

public:
    except_impl(Seq2&& seq2, Pred&& pred)
        : seq2_(std::forward<Seq2>(seq2)), pred_(std::forward<Pred>(pred))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    // Movable but not copyable
    except_impl(const except_impl&) = delete;
    except_impl(except_impl&&) = default;
    except_impl& operator=(const except_impl&) = delete;
    except_impl& operator=(except_impl&&) = default;

    template<typename Seq1>
    auto operator()(Seq1&& seq1)
        -> coveo::enumerable<typename seq_traits<Seq1>::raw_value_type>
    {
        // Note: if seq2_ and/or pred_ are not refs, they will be moved
        // by the forward below and become invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        // #clp TODO what if one or both sequences are already sorted?
        return next_impl<Seq1>(std::forward<Seq1>(seq1),
                               std::forward<Seq2>(seq2_),
                               std::forward<Pred>(pred_));
    }
};

// Implement of group_by operator
template<typename KeySelector,
         typename ValueSelector,
         typename ResultSelector,
         typename Pred>
class group_by_impl
{
public:
    // Implementation of next delegate that returns group information
    template<typename Seq>
    class next_impl
    {
    public:
        // Key and value types returned by selectors.
        typedef decltype(std::declval<KeySelector>()(std::declval<typename seq_traits<Seq>::const_reference>()))    key;
        typedef decltype(std::declval<ValueSelector>()(std::declval<typename seq_traits<Seq>::const_reference>()))  value;

        // Vector of values sharing a common key.
        typedef std::vector<std::decay_t<value>>    value_v;
        typedef decltype(coveo::enumerate_container(std::declval<const value_v&>()))
                                                    values;

        // Map that stores keys and their corresponding values.
        typedef std::map<std::decay_t<key>, value_v, proxy_cmp<Pred>> values_by_key_m;

        // Result returned by result selector.
        typedef decltype(std::declval<ResultSelector>()(std::declval<key>(), std::declval<values>())) result;

        // Vector of results returned by this next delegate.
        typedef std::vector<std::decay_t<result>> result_v;

    private:
        // Bean storing group information. Shared among delegates in a shared_ptr.
        class groups_info
        {
        private:
            Seq seq_;                       // Sequence containing the elements
            KeySelector key_sel_;           // Returns keys for elements
            ValueSelector value_sel_;       // Returns values for elements
            ResultSelector result_sel_;     // Converts groups into end results
            Pred pred_;                     // Compares keys
            result_v results_;              // Vector of end results
            bool init_flag_;                // Whether results_ has been initialized

        public:
            groups_info(Seq&& seq, KeySelector&& key_sel, ValueSelector&& value_sel,
                        ResultSelector&& result_sel, Pred&& pred)
                : seq_(std::forward<Seq>(seq)),
                  key_sel_(std::forward<KeySelector>(key_sel)),
                  value_sel_(std::forward<ValueSelector>(value_sel)),
                  result_sel_(std::forward<ResultSelector>(result_sel)),
                  pred_(std::forward<Pred>(pred)),
                  results_(), init_flag_(false) { }

            // Not copyable/movable, stored in a shared_ptr
            groups_info(const groups_info&) = delete;
            groups_info& operator=(const groups_info&) = delete;

            const result_v& get_results() {
                // Init everything on first call
                if (!init_flag_) {
                    // First build map of groups
                    values_by_key_m groups{proxy_cmp<Pred>(pred_)};
                    for (auto&& obj : seq_) {
                        groups[key_sel_(obj)].emplace_back(value_sel_(obj));
                    }

                    // Now build vector of results
                    // Note that since we no longer need the map afterwards, we can actually move
                    // the vectors stored as map values into the results vector.
                    results_.reserve(groups.size());
                    for (const auto& group_pair : groups) {
                        results_.emplace_back(result_sel_(group_pair.first,
                                                          coveo::enumerate_container(std::move(group_pair.second))));
                    }

                    init_flag_ = true;
                }
                return results_;
            }
        };
        typedef std::shared_ptr<groups_info> groups_info_sp;

        groups_info_sp spgroups_;                   // Information about groups
        typename result_v::const_iterator icurr_;   // Iterator pointing at current result.
        typename result_v::const_iterator iendr_;   // Iterator pointing at end of result vector.
        bool init_flag_;                            // Whether icurr_ and iendr_ have been initialized

    public:
        next_impl(Seq&& seq, KeySelector&& key_sel, ValueSelector&& value_sel,
                  ResultSelector&& result_sel, Pred&& pred)
            : spgroups_(std::make_shared<groups_info>(std::forward<Seq>(seq),
                                                      std::forward<KeySelector>(key_sel),
                                                      std::forward<ValueSelector>(value_sel),
                                                      std::forward<ResultSelector>(result_sel),
                                                      std::forward<Pred>(pred))),
              icurr_(), iendr_(), init_flag_(false) { }

        template<typename Op>
        auto operator()(Op&) -> typename seq_traits<result_v>::const_pointer {
            // Init iterators on first call
            if (!init_flag_) {
                const auto& results = spgroups_->get_results();
                icurr_ = std::begin(results);
                iendr_ = std::end(results);
                init_flag_ = true;
            }
            typename seq_traits<result_v>::const_pointer pobj = nullptr;
            if (icurr_ != iendr_) {
                typename seq_traits<result_v>::const_reference robj = *icurr_++;
                pobj = std::addressof(robj);
            }
            return pobj;
        }
    };

private:
    KeySelector key_sel_;       // Selector that provides keys for each element
    ValueSelector value_sel_;   // Selector that provides values for each element
    ResultSelector result_sel_; // Selector that converts each group into a final result
    Pred pred_;                 // Predicate used to compare keys
#ifdef _DEBUG
    bool applied_;              // Tracks operator application
#endif

public:
    group_by_impl(KeySelector&& key_sel, ValueSelector&& value_sel,
                  ResultSelector&& result_sel, Pred&& pred)
        : key_sel_(std::forward<KeySelector>(key_sel)),
          value_sel_(std::forward<ValueSelector>(value_sel)),
          result_sel_(std::forward<ResultSelector>(result_sel)),
          pred_(std::forward<Pred>(pred))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    // Movable but not copyable
    group_by_impl(const group_by_impl&) = delete;
    group_by_impl(group_by_impl&&) = default;
    group_by_impl& operator=(const group_by_impl&) = delete;
    group_by_impl& operator=(group_by_impl&&) = default;

    template<typename Seq>
    auto operator()(Seq&& seq)
        -> coveo::enumerable<typename seq_traits<typename next_impl<Seq>::result_v>::raw_value_type>
    {
        // Note: if members are not refs, they will be moved by the forwards
        // below and become invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        return next_impl<Seq>(std::forward<Seq>(seq),
                              std::forward<KeySelector>(key_sel_),
                              std::forward<ValueSelector>(value_sel_),
                              std::forward<ResultSelector>(result_sel_),
                              std::forward<Pred>(pred_));
    }
};

// Implementation of group_join operator
template<typename InnerSeq,
         typename OuterKeySelector,
         typename InnerKeySelector,
         typename ResultSelector,
         typename Pred>
class group_join_impl
{
public:
    // Implementation of next delegate that returns final results
    template<typename OuterSeq>
    class next_impl
    {
    public:
        // Key returned by key selectors.
        typedef decltype(std::declval<OuterKeySelector>()(std::declval<typename seq_traits<OuterSeq>::const_reference>())) key;

        // Group of elements from the inner sequence that share a common key.
        typedef std::vector<typename seq_traits<InnerSeq>::raw_value_type>                      inner_element_v;
        typedef decltype(coveo::enumerate_container(std::declval<const inner_element_v&>()))    inner_elements;
    
        // Result returned by result selector.
        typedef decltype(std::declval<ResultSelector>()(std::declval<typename seq_traits<OuterSeq>::const_reference>(),
                                                        std::declval<inner_elements>()))        result;

        // Vector of results returned by this next delegate.
        typedef std::vector<std::decay_t<result>> result_v;

    private:
        // Bean storing groups information. Shared among delegates in a shared_ptr.
        class groups_info
        {
        private:
            OuterSeq outer_seq_;                // Outer sequence.
            InnerSeq inner_seq_;                // Inner sequence from which to create groups.
            OuterKeySelector outer_key_sel_;    // Key selector for outer sequence.
            InnerKeySelector inner_key_sel_;    // Key selector for inner sequence.
            ResultSelector result_sel_;         // Selector converting groups into final results.
            Pred pred_;                         // Predicate used to compare keys.
            result_v results_;                  // Vector of final results.
            bool init_flag_;                    // Whether results_ has been initialized.

        public:
            groups_info(OuterSeq&& outer_seq, InnerSeq&& inner_seq,
                        OuterKeySelector&& outer_key_sel, InnerKeySelector&& inner_key_sel,
                        ResultSelector&& result_sel, Pred&& pred)
                : outer_seq_(std::forward<OuterSeq>(outer_seq)),
                  inner_seq_(std::forward<InnerSeq>(inner_seq)),
                  outer_key_sel_(std::forward<OuterKeySelector>(outer_key_sel)),
                  inner_key_sel_(std::forward<InnerKeySelector>(inner_key_sel)),
                  result_sel_(std::forward<ResultSelector>(result_sel)),
                  pred_(std::forward<Pred>(pred)),
                  results_(), init_flag_(false) { }

            // Not copyable/movable, stored in a shared_ptr
            groups_info(const groups_info&) = delete;
            groups_info& operator=(const groups_info&) = delete;

            const result_v& get_results() {
                // Init results on first call.
                if (!init_flag_) {
                    // Build map of groups of elements from inner sequence.
                    std::map<std::decay_t<key>, inner_element_v, proxy_cmp<Pred>> keyed_inner_elems{proxy_cmp<Pred>(pred_)};
                    for (auto&& inner_elem : inner_seq_) {
                        keyed_inner_elems[inner_key_sel_(inner_elem)].emplace_back(inner_elem);
                    }

                    // Iterate outer sequence and build final results by matching the elements with
                    // the groups we built earlier.
                    // #clp TODO reserve space in results_ if outer seq's iterators are random-access
                    auto iendki = keyed_inner_elems.end();
                    for (auto&& outer_elem : outer_seq_) {
                        key outer_key = outer_key_sel_(outer_elem);
                        auto icurki = keyed_inner_elems.find(outer_key);
                        inner_elements inner_elems = icurki != iendki ? coveo::enumerate_container(icurki->second)
                                                                      : inner_elements::empty();
                        results_.emplace_back(result_sel_(outer_elem, inner_elems));
                    }

                    init_flag_ = true;
                }
                return results_;
            }
        };
        typedef std::shared_ptr<groups_info> groups_info_sp;

        groups_info_sp spgroups_;                   // Information about groups and results
        typename result_v::const_iterator icurr_;   // Iterator pointing at current result.
        typename result_v::const_iterator iendr_;   // Iterator pointing at end of result vector.
        bool init_flag_;                            // Whether icurr_/iendr_ have been initialized.

    public:
        next_impl(OuterSeq&& outer_seq, InnerSeq&& inner_seq,
                  OuterKeySelector&& outer_key_sel, InnerKeySelector&& inner_key_sel,
                  ResultSelector&& result_sel, Pred&& pred)
            : spgroups_(std::make_shared<groups_info>(std::forward<OuterSeq>(outer_seq),
                                                      std::forward<InnerSeq>(inner_seq),
                                                      std::forward<OuterKeySelector>(outer_key_sel),
                                                      std::forward<InnerKeySelector>(inner_key_sel),
                                                      std::forward<ResultSelector>(result_sel),
                                                      std::forward<Pred>(pred))),
              icurr_(), iendr_(), init_flag_(false) { }

        template<typename Op>
        auto operator()(Op&) -> typename seq_traits<result_v>::const_pointer {
            // Init iterators on first call
            if (!init_flag_) {
                const auto& results = spgroups_->get_results();
                icurr_ = std::begin(results);
                iendr_ = std::end(results);
                init_flag_ = true;
            }
            typename seq_traits<result_v>::const_pointer pobj = nullptr;
            if (icurr_ != iendr_) {
                typename seq_traits<result_v>::const_reference robj = *icurr_++;
                pobj = std::addressof(robj);
            }
            return pobj;
        }
    };

private:
    InnerSeq inner_seq_;                // Inner sequence whose elements to group with outer elements with matching keys
    OuterKeySelector outer_key_sel_;    // Selector to get keys from elements in the outer sequence
    InnerKeySelector inner_key_sel_;    // Selector to get keys from elements in the inner sequence
    ResultSelector result_sel_;         // Selector to convert groups into final results
    Pred pred_;                         // Predicate used to compare keys
#ifdef _DEBUG
    bool applied_;                      // Tracks operator application
#endif

public:
    group_join_impl(InnerSeq&& inner_seq,
                    OuterKeySelector&& outer_key_sel,
                    InnerKeySelector&& inner_key_sel,
                    ResultSelector&& result_sel,
                    Pred&& pred)
        : inner_seq_(std::forward<InnerSeq>(inner_seq)),
          outer_key_sel_(std::forward<OuterKeySelector>(outer_key_sel)),
          inner_key_sel_(std::forward<InnerKeySelector>(inner_key_sel)),
          result_sel_(std::forward<ResultSelector>(result_sel)),
          pred_(std::forward<Pred>(pred))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    // Movable but not copyable
    group_join_impl(const group_join_impl&) = delete;
    group_join_impl(group_join_impl&&) = default;
    group_join_impl& operator=(const group_join_impl&) = delete;
    group_join_impl& operator=(group_join_impl&&) = default;

    template<typename OuterSeq>
    auto operator()(OuterSeq&& outer_seq)
        -> coveo::enumerable<typename seq_traits<typename next_impl<OuterSeq>::result_v>::raw_value_type>
    {
        // Note: if members are not refs, they will be moved by the forwards
        // below and become invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        return next_impl<OuterSeq>(std::forward<OuterSeq>(outer_seq),
                                   std::forward<InnerSeq>(inner_seq_),
                                   std::forward<OuterKeySelector>(outer_key_sel_),
                                   std::forward<InnerKeySelector>(inner_key_sel_),
                                   std::forward<ResultSelector>(result_sel_),
                                   std::forward<Pred>(pred_));
    }
};

// Implementation of intersect operator
template<typename Seq2, typename Pred>
class intersect_impl
{
public:
    // Implementation of next delegate that performs intersection
    template<typename Seq1>
    class next_impl
    {
    public:
        // Vector storing the elements.
        typedef std::vector<typename seq_traits<Seq2>::raw_value_type> seq2_element_v;

        // Type of iterators for the sequences.
        typedef typename seq_traits<Seq1>::iterator_type    first_iterator_type;
        typedef typename seq_traits<Seq2>::iterator_type    second_iterator_type;

    private:
        // Information about intersection. Shared among delegates through smart_ptr.
        class intersect_info
        {
        private:
            Seq1 seq1_;                     // First sequence to intersect.
            first_iterator_type iend1_;     // Iterator pointing at end of first sequence.
            Seq2 seq2_;                     // Second sequence to intersect.
            Pred pred_;                     // Predicate used to compare elements.
            seq2_element_v v_in_seq2_;      // Vector of elements from second sequence.
            bool init_flag_;                // Whether v_in_seq2_ has been initialized.

        public:
            intersect_info(Seq1&& seq1, Seq2&& seq2, Pred&& pred)
                : seq1_(std::forward<Seq1>(seq1)),
                  iend1_(std::end(seq1_)),
                  seq2_(std::forward<Seq2>(seq2)),
                  pred_(std::forward<Pred>(pred)),
                  v_in_seq2_(),
                  init_flag_(false) { }

            // Not copyable/movable, stored in a shared_ptr
            intersect_info(const intersect_info&) = delete;
            intersect_info& operator=(const intersect_info&) = delete;

            first_iterator_type first_begin() {
                return std::begin(seq1_);
            }

            bool is_in_seq2(typename seq_traits<Seq1>::const_reference obj) {
                // Build vector of second sequence on the first call.
                if (!init_flag_) {
                    // Add all elements from second sequence to a vector and sort it.
                    // #clp TODO reserve if sequence has random-access iterators
                    v_in_seq2_ = seq2_element_v(std::begin(seq2_), std::end(seq2_));
                    std::sort(v_in_seq2_.begin(), v_in_seq2_.end(), pred_);
                    init_flag_ = true;
                }
                return std::binary_search(v_in_seq2_.cbegin(), v_in_seq2_.cend(), obj, pred_);
            }

            // Returns next element that is both in first and second sequence or nullptr when done
            auto get_next(first_iterator_type& icur1)
                -> typename seq_traits<Seq1>::const_pointer
            {
                typename seq_traits<Seq1>::const_pointer pobj = nullptr;
                for (; pobj == nullptr && icur1 != iend1_; ++icur1) {
                    typename seq_traits<Seq1>::const_reference robj = *icur1;
                    if (is_in_seq2(robj)) {
                        pobj = std::addressof(robj);
                    }
                }
                return pobj;
            }
        };
        typedef std::shared_ptr<intersect_info> intersect_info_sp;

        intersect_info_sp spint_info_;      // Intersection information.
        first_iterator_type icur_;          // Current position in first sequence.

    public:
        next_impl(Seq1&& seq1, Seq2&& seq2, Pred&& pred)
            : spint_info_(std::make_shared<intersect_info>(std::forward<Seq1>(seq1),
                                                           std::forward<Seq2>(seq2),
                                                           std::forward<Pred>(pred))),
              icur_(spint_info_->first_begin()) { }

        template<typename Op>
        auto operator()(Op&) {
            return spint_info_->get_next(icur_);
        }
    };

private:
    Seq2 seq2_;     // Second sequence to intersect.
    Pred pred_;     // Predicate used to compare elements.
#ifdef _DEBUG
    bool applied_;  // Tracks operator application
#endif

public:
    intersect_impl(Seq2&& seq2, Pred&& pred)
        : seq2_(std::forward<Seq2>(seq2)),
          pred_(std::forward<Pred>(pred))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    // Movable but not copyable
    intersect_impl(const intersect_impl&) = delete;
    intersect_impl(intersect_impl&&) = default;
    intersect_impl& operator=(const intersect_impl&) = delete;
    intersect_impl& operator=(intersect_impl&&) = default;

    template<typename Seq1>
    auto operator()(Seq1&& seq1)
        -> coveo::enumerable<typename seq_traits<Seq1>::raw_value_type>
    {
        // Note: if seq2_ and/or pred_ are not refs, they will be moved
        // by the forward below and become invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        // #clp TODO what if one or both sequences are already sorted?
        return next_impl<Seq1>(std::forward<Seq1>(seq1),
                               std::forward<Seq2>(seq2_),
                               std::forward<Pred>(pred_));
    }
};

// Implementation of join operator
template<typename InnerSeq,
         typename OuterKeySelector,
         typename InnerKeySelector,
         typename ResultSelector,
         typename Pred>
class join_impl
{
public:
    // Implementation of next delegate that performs join
    template<typename OuterSeq>
    class next_impl
    {
    public:
        // Key returned by key selectors.
        typedef decltype(std::declval<OuterKeySelector>()(std::declval<typename seq_traits<OuterSeq>::const_reference>()))  key;

        // Group of elements from the inner sequence that share a common key.
        typedef std::vector<typename seq_traits<InnerSeq>::raw_value_type> inner_element_v;
    
        // Result returned by result selector.
        typedef decltype(std::declval<ResultSelector>()(std::declval<typename seq_traits<OuterSeq>::const_reference>(),
                                                        std::declval<typename seq_traits<InnerSeq>::const_reference>()))    result;

        // Vector of results returned by this next delegate.
        typedef std::vector<std::decay_t<result>> result_v;

    private:
        // Bean storing join information. Shared among delegates in a shared_ptr.
        class join_info
        {
        private:
            OuterSeq outer_seq_;                // Outer sequence.
            InnerSeq inner_seq_;                // Inner sequence to join with outer sequence.
            OuterKeySelector outer_key_sel_;    // Key selector for outer sequence.
            InnerKeySelector inner_key_sel_;    // Key selector for inner sequence.
            ResultSelector result_sel_;         // Selector converting joined elements into final results.
            Pred pred_;                         // Predicate used to compare keys.
            result_v results_;                  // Vector of final results.
            bool init_flag_;                    // Whether results_ has been initialized.

        public:
            join_info(OuterSeq&& outer_seq, InnerSeq&& inner_seq,
                      OuterKeySelector&& outer_key_sel, InnerKeySelector&& inner_key_sel,
                      ResultSelector&& result_sel, Pred&& pred)
                : outer_seq_(std::forward<OuterSeq>(outer_seq)),
                  inner_seq_(std::forward<InnerSeq>(inner_seq)),
                  outer_key_sel_(std::forward<OuterKeySelector>(outer_key_sel)),
                  inner_key_sel_(std::forward<InnerKeySelector>(inner_key_sel)),
                  result_sel_(std::forward<ResultSelector>(result_sel)),
                  pred_(std::forward<Pred>(pred)),
                  results_(), init_flag_(false) { }

            // Not copyable/movable, stored in a shared_ptr
            join_info(const join_info&) = delete;
            join_info& operator=(const join_info&) = delete;

            const result_v& get_results() {
                // Init results on first call.
                if (!init_flag_) {
                    // Build map of groups of elements from inner sequence.
                    std::map<std::decay_t<key>, inner_element_v, proxy_cmp<Pred>> keyed_inner_elems{proxy_cmp<Pred>(pred_)};
                    for (auto&& inner_elem : inner_seq_) {
                        keyed_inner_elems[inner_key_sel_(inner_elem)].emplace_back(inner_elem);
                    }

                    // Iterate outer sequence and build final results by joining the elements with
                    // those in the groups we built earlier.
                    // #clp TODO reserve space in results_ if sequences' iterators are random-access?
                    auto iendki = keyed_inner_elems.end();
                    for (auto&& outer_elem : outer_seq_) {
                        key outer_key = outer_key_sel_(outer_elem);
                        auto icurki = keyed_inner_elems.find(outer_key);
                        if (icurki != iendki) {
                            for (auto&& inner_elem : icurki->second) {
                                results_.emplace_back(result_sel_(outer_elem, inner_elem));
                            }
                        }
                    }

                    init_flag_ = true;
                }
                return results_;
            }
        };
        typedef std::shared_ptr<join_info> join_info_sp;

        join_info_sp spjoin_info_;                  // Information about joined elements and results
        typename result_v::const_iterator icurr_;   // Iterator pointing at current result.
        typename result_v::const_iterator iendr_;   // Iterator pointing at end of result vector.
        bool init_flag_;                            // Whether icurr_/iendr_ have been initialized.

    public:
        next_impl(OuterSeq&& outer_seq, InnerSeq&& inner_seq,
                  OuterKeySelector&& outer_key_sel, InnerKeySelector&& inner_key_sel,
                  ResultSelector&& result_sel, Pred&& pred)
            : spjoin_info_(std::make_shared<join_info>(std::forward<OuterSeq>(outer_seq),
                                                       std::forward<InnerSeq>(inner_seq),
                                                       std::forward<OuterKeySelector>(outer_key_sel),
                                                       std::forward<InnerKeySelector>(inner_key_sel),
                                                       std::forward<ResultSelector>(result_sel),
                                                       std::forward<Pred>(pred))),
              icurr_(), iendr_(), init_flag_(false) { }

        template<typename Op>
        auto operator()(Op&) -> typename seq_traits<result_v>::const_pointer {
            // Init iterators on first call
            if (!init_flag_) {
                const auto& results = spjoin_info_->get_results();
                icurr_ = std::begin(results);
                iendr_ = std::end(results);
                init_flag_ = true;
            }
            typename seq_traits<result_v>::const_pointer pobj = nullptr;
            if (icurr_ != iendr_) {
                typename seq_traits<result_v>::const_reference robj = *icurr_++;
                pobj = std::addressof(robj);
            }
            return pobj;
        }
    };

private:
    InnerSeq inner_seq_;                // Inner sequence to join.
    OuterKeySelector outer_key_sel_;    // Fetches keys for elements of outer sequence.
    InnerKeySelector inner_key_sel_;    // Fetches keys for elements of inner sequence.
    ResultSelector result_sel_;         // Creates results from joined elements.
    Pred pred_;                         // Predicate to compare keys.
#ifdef _DEBUG
    bool applied_;                      // Tracks operator application.
#endif

public:
    join_impl(InnerSeq&& inner_seq,
              OuterKeySelector&& outer_key_sel,
              InnerKeySelector&& inner_key_sel,
              ResultSelector&& result_sel,
              Pred&& pred)
        : inner_seq_(std::forward<InnerSeq>(inner_seq)),
          outer_key_sel_(std::forward<OuterKeySelector>(outer_key_sel)),
          inner_key_sel_(std::forward<InnerKeySelector>(inner_key_sel)),
          result_sel_(std::forward<ResultSelector>(result_sel)),
          pred_(std::forward<Pred>(pred))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    // Movable but not copyable
    join_impl(const join_impl&) = delete;
    join_impl(join_impl&&) = default;
    join_impl& operator=(const join_impl&) = delete;
    join_impl& operator=(join_impl&&) = default;

    template<typename OuterSeq>
    auto operator()(OuterSeq&& outer_seq)
        -> coveo::enumerable<typename seq_traits<typename next_impl<OuterSeq>::result_v>::raw_value_type>
    {
        // Note: if members are not refs, they will be moved by the forwards
        // below and become invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        return next_impl<OuterSeq>(std::forward<OuterSeq>(outer_seq),
                                   std::forward<InnerSeq>(inner_seq_),
                                   std::forward<OuterKeySelector>(outer_key_sel_),
                                   std::forward<InnerKeySelector>(inner_key_sel_),
                                   std::forward<ResultSelector>(result_sel_),
                                   std::forward<Pred>(pred_));
    }
};

// Implementation of last operator (version without argument)
class last_impl_0
{
private:
    // If we have bidi iterators, we can simply use rbegin
    template<typename Seq>
    decltype(auto) impl(Seq&& seq, std::bidirectional_iterator_tag) {
        auto ricur = std::rbegin(seq);
        if (ricur == std::rend(seq)) {
            throw_linq_empty_sequence();
        }
        return *ricur;
    }

    // Otherwise we'll have to be creative
    template<typename Seq>
    decltype(auto) impl(Seq&& seq, std::forward_iterator_tag) {
        auto icur = std::begin(seq);
        auto iend = std::end(seq);
        if (icur == iend) {
            throw_linq_empty_sequence();
        }
        decltype(icur) iprev;
        while (icur != iend) {
            iprev = icur;
            ++icur;
        }
        return *iprev;
    }

public:
    template<typename Seq>
    decltype(auto) operator()(Seq&& seq) {
        return impl(std::forward<Seq>(seq),
            typename std::iterator_traits<typename seq_traits<Seq>::iterator_type>::iterator_category());
    }
};

// Implementation of last operator (version with predicate)
template<typename Pred>
class last_impl_1
{
private:
    const Pred& pred_;  // Predicate to satisfy

private:
    // If we have bidi iterators, we can simply use rbegin
    template<typename Seq>
    decltype(auto) impl(Seq&& seq, std::bidirectional_iterator_tag) {
        auto ricur = std::rbegin(seq);
        auto riend = std::rend(seq);
        if (ricur == riend) {
            throw_linq_empty_sequence();
        }
        // #clp TODO what if sequence is sorted? Can we optimize?
        auto rifound = std::find_if(ricur, riend, pred_);
        if (rifound == riend) {
            throw_linq_out_of_range();
        }
        return *rifound;
    }

    // Otherwise we'll have to be creative
    template<typename Seq>
    decltype(auto) impl(Seq&& seq, std::forward_iterator_tag) {
        auto icur = std::begin(seq);
        auto iend = std::end(seq);
        if (icur == iend) {
            throw_linq_empty_sequence();
        }
        auto ifound = iend;
        while (icur != iend) {
            if (pred_(*icur)) {
                ifound = icur;
            }
            ++icur;
        }
        if (ifound == iend) {
            throw_linq_out_of_range();
        }
        return *ifound;
    }

public:
    explicit last_impl_1(const Pred& pred)
        : pred_(pred) { }

    template<typename Seq>
    decltype(auto) operator()(Seq&& seq) {
        return impl(std::forward<Seq>(seq),
            typename std::iterator_traits<typename seq_traits<Seq>::iterator_type>::iterator_category());
    }
};

// Implementation of last_or_default operator (version without argument)
class last_or_default_impl_0
{
private:
    // If we have bidi iterators, we can simply use rbegin
    template<typename Seq>
    auto impl(Seq&& seq, std::bidirectional_iterator_tag) -> typename seq_traits<Seq>::raw_value_type {
        auto ricur = std::rbegin(seq);
        return ricur != std::rend(seq) ? *ricur : typename seq_traits<Seq>::raw_value_type();
    }

    // Otherwise we'll have to be creative
    template<typename Seq>
    auto impl(Seq&& seq, std::forward_iterator_tag) -> typename seq_traits<Seq>::raw_value_type {
        auto icur = std::begin(seq);
        auto iend = std::end(seq);
        auto iprev = iend;
        while (icur != iend) {
            iprev = icur;
            ++icur;
        }
        return iprev != iend ? *iprev : typename seq_traits<Seq>::raw_value_type();
    }

public:
    template<typename Seq>
    auto operator()(Seq&& seq) -> typename seq_traits<Seq>::raw_value_type {
        return impl(std::forward<Seq>(seq),
            typename std::iterator_traits<typename seq_traits<Seq>::iterator_type>::iterator_category());
    }
};

// Implementation of last_or_default operator (version with predicate)
template<typename Pred>
class last_or_default_impl_1
{
private:
    const Pred& pred_;  // Predicate to satisfy

private:
    // If we have bidi iterators, we can simply use rbegin
    template<typename Seq>
    auto impl(Seq&& seq, std::bidirectional_iterator_tag) -> typename seq_traits<Seq>::raw_value_type {
        auto riend = std::rend(seq);
        auto rifound = std::find_if(std::rbegin(seq), riend, pred_);
        return rifound != riend ? *rifound : typename seq_traits<Seq>::raw_value_type();
    }

    // Otherwise we'll have to be creative
    template<typename Seq>
    auto impl(Seq&& seq, std::forward_iterator_tag) -> typename seq_traits<Seq>::raw_value_type {
        auto icur = std::begin(seq);
        auto iend = std::end(seq);
        auto ifound = iend;
        while (icur != iend) {
            if (pred_(*icur)) {
                ifound = icur;
            }
            ++icur;
        }
        return ifound != iend ? *ifound : typename seq_traits<Seq>::raw_value_type();
    }

public:
    explicit last_or_default_impl_1(const Pred& pred)
        : pred_(pred) { }

    template<typename Seq>
    auto operator()(Seq&& seq) -> typename seq_traits<Seq>::raw_value_type {
        return impl(std::forward<Seq>(seq),
            typename std::iterator_traits<typename seq_traits<Seq>::iterator_type>::iterator_category());
    }
};

// Implementation of a comparator using KeySelector and Pred.
// Has an operator() that returns an int representing the comparison
// between two objects (negative values means left is before right, etc.)
template<typename KeySelector,
         typename Pred,
         bool Descending,
         int _LessValue = Descending ? 1 : -1>
class order_by_comparator
{
private:
    KeySelector key_sel_;   // Key selector used to fetch keys from elements.
    Pred pred_;             // Predicate used to compare keys.

public:
    order_by_comparator(KeySelector&& key_sel, Pred&& pred)
        : key_sel_(std::forward<KeySelector>(key_sel)), pred_(std::forward<Pred>(pred)) { }

    // Cannot copy/move, stored in a unique_ptr
    order_by_comparator(const order_by_comparator&) = delete;
    order_by_comparator& operator=(const order_by_comparator&) = delete;

    // Compares two values, returning relative position of the two in an ordered sequence.
    template<typename T1, typename T2>
    int operator()(T1&& left, T2&& right) const {
        decltype(auto) leftk = key_sel_(left);
        decltype(auto) rightk = key_sel_(right);
        int cmp;
        if (pred_(leftk, rightk)) {
            cmp = _LessValue;
        } else if (pred_(rightk, leftk)) {
            cmp = -_LessValue;
        } else {
            // Keys are equal.
            cmp = 0;
        }
        return cmp;
    }
};

// Implementation of a comparator that calls two comparators in order.
// If the first comparator returns 0, calls the second comparator.
template<typename Cmp1, typename Cmp2>
class dual_order_by_comparator
{
private:
    std::unique_ptr<Cmp1> upcmp1_;  // First comparator called.
    std::unique_ptr<Cmp2> upcmp2_;  // Second comparator called.

public:
    dual_order_by_comparator(std::unique_ptr<Cmp1>&& upcmp1, std::unique_ptr<Cmp2>&& upcmp2)
        : upcmp1_(std::move(upcmp1)), upcmp2_(std::move(upcmp2)) { }

    // Cannot copy/move, stored in a unique_ptr
    dual_order_by_comparator(const dual_order_by_comparator&) = delete;
    dual_order_by_comparator& operator=(const dual_order_by_comparator&) = delete;

    // Compares two values by calling first then second comparator.
    template<typename T1, typename T2>
    int operator()(T1&& left, T2&& right) const {
        int cmp = (*upcmp1_)(left, right);
        if (cmp == 0) {
            cmp = (*upcmp2_)(left, right);
        }
        return cmp;
    }
};

// Forward declaration to declare friendship
template<typename Cmp> class order_by_impl;

// Implementation of order_by/then_by operators.
// This is the operator with a sequence.
template<typename Seq, typename Cmp>
class order_by_impl_with_seq
{
    // We need this friendship so that it can "steal" our internals.
    template<typename> friend class order_by_impl;

private:
    Seq seq_;                                                           // Sequence we're ordering.
    std::unique_ptr<Cmp> upcmp_;                                        // Comparator used to order a sequence.
    coveo::enumerable<typename seq_traits<Seq>::raw_value_type> enum_;  // Enumerator of ordered elements.
    bool init_flag_;                                                    // Whether enum_ has been initialized.

    // Called to initialize enum_ before using it.
    void init() {
        auto spordered = std::make_shared<std::vector<typename seq_traits<Seq>::raw_value_type>>();
        // #clp TODO reserve if Seq has random-access iterators
        for (auto&& elem : seq_) {
            spordered->push_back(elem);
        }
        std::stable_sort(std::begin(*spordered), std::end(*spordered), [this](auto&& left, auto&& right) {
            return (*upcmp_)(left, right) < 0;
        });
        enum_ = coveo::enumerable<typename seq_traits<Seq>::raw_value_type>(
            [spordered, it = std::begin(*spordered), end = std::end(*spordered)](auto&&) mutable {
                return it != end ? std::addressof(*it++) : nullptr;
            });
        init_flag_ = true;
    }

public:
    // Type of iterator used for the ordered sequence.
    typedef typename coveo::enumerable<typename seq_traits<Seq>::raw_value_type>::const_iterator const_iterator;

    // Constructor called by the impl without sequence.
    order_by_impl_with_seq(Seq&& seq, std::unique_ptr<Cmp>&& upcmp)
        : seq_(std::forward<Seq>(seq)), upcmp_(std::move(upcmp)), enum_(), init_flag_(false) { }

    // Movable but not copyable
    order_by_impl_with_seq(const order_by_impl_with_seq&) = delete;
    order_by_impl_with_seq(order_by_impl_with_seq&&) = default;
    order_by_impl_with_seq& operator=(const order_by_impl_with_seq&) = delete;
    order_by_impl_with_seq& operator=(order_by_impl_with_seq&&) = default;

    // Support for ordered sequence.
    const_iterator begin() {
        if (!init_flag_) {
            init();
        }
        return enum_.begin();
    }
    const_iterator end() {
        if (!init_flag_) {
            init();
        }
        return enum_.end();
    }
};

// Implementation of order_by/then_by operators.
// This is the "initial" operator without a sequence, as returned by helper methods.
template<typename Cmp>
class order_by_impl
{
    // Type trait used to identify impls with sequences, to differenciate them from
    // the actual sequences we'll be receiving.
    template<typename> struct is_order_by_impl_with_seq : std::false_type { };
    template<typename _Seq, typename _Cmp>
    struct is_order_by_impl_with_seq<order_by_impl_with_seq<_Seq, _Cmp>> : std::true_type { };

private:
    std::unique_ptr<Cmp> upcmp_;    // Comparator used to order a sequence.

public:
    explicit order_by_impl(std::unique_ptr<Cmp>&& upcmp)
        : upcmp_(std::move(upcmp)) { }

    // Movable by not copyable
    order_by_impl(const order_by_impl&) = delete;
    order_by_impl(order_by_impl&&) = default;
    order_by_impl& operator=(const order_by_impl&) = delete;
    order_by_impl& operator=(order_by_impl&&) = default;

    // When applied to a sequence, produces a different object.
    template<typename Seq>
    auto operator()(Seq&& seq) {
        // Cannot apply twice.
        assert(upcmp_);

        return order_by_impl_with_seq<Seq, Cmp>(std::forward<Seq>(seq), std::move(upcmp_));
    }

    // When applied to an impl with sequence, merges the two and chains the comparators.
    template<typename ImplSeq, typename ImplCmp>
    auto operator()(order_by_impl_with_seq<ImplSeq, ImplCmp>&& impl) {
        typedef dual_order_by_comparator<ImplCmp, Cmp> dual_comparator;
        return order_by_impl_with_seq<ImplSeq, dual_comparator>(std::move(impl.seq_),
                                                                std::make_unique<dual_comparator>(std::move(impl.upcmp_),
                                                                                                  std::move(upcmp_)));
    }
};

// Implementation of reverse operator
class reverse_impl
{
private:
    // If we have bidi iterators, we can simply use rbegin
    template<typename Seq>
    auto impl(Seq&& seq, std::bidirectional_iterator_tag)
        -> coveo::enumerable<typename seq_traits<Seq>::raw_value_type>
    {
        return coveo::enumerable<typename seq_traits<Seq>::raw_value_type>::for_range(
            std::rbegin(seq), std::rend(seq));
    }

    // Otherwise we'll have to be creative
    template<typename Seq>
    auto impl(Seq&& seq, std::forward_iterator_tag)
        -> coveo::enumerable<typename seq_traits<Seq>::raw_value_type>
    {
        auto spvpelems = std::make_shared<std::vector<typename seq_traits<Seq>::raw_value_type>>();
        for (auto&& elem : seq) {
            spvpelems->push_back(elem);
        }
        std::reverse(spvpelems->begin(), spvpelems->end());
        return [spvpelems, it = std::begin(*spvpelems), end = std::end(*spvpelems)](auto&&) mutable {
            return it != end ? std::addressof(*it++) : nullptr;
        };
    }

public:
    template<typename Seq>
    auto operator()(Seq&& seq)
        -> coveo::enumerable<typename seq_traits<Seq>::raw_value_type>
    {
        return impl(std::forward<Seq>(seq),
            typename std::iterator_traits<typename seq_traits<Seq>::iterator_type>::iterator_category());
    }
};

// Implementation of select and select_with_index operators
template<typename Selector>
class select_impl
{
public:
    // Next delegate implementation for select operator
    template<typename Seq, typename SelectorRes, typename U>
    class next_impl
    {
    private:
        // Iterator used by the sequence.
        typedef typename seq_traits<Seq>::iterator_type iterator_type;

        // Bean storing info about elements. Shared among delegates.
        struct select_info {
            Seq seq_;               // Sequence being transformed.
            iterator_type iend_;    // Iterator pointing at end of seq_.
            Selector sel_;          // Selector transforming the elements.

            select_info(Seq&& seq, Selector&& sel)
                : seq_(std::forward<Seq>(seq)),
                  iend_(std::end(seq_)),
                  sel_(std::forward<Selector>(sel)) { }

            // Cannot copy/move, stored in a shared_ptr
            select_info(const select_info&) = delete;
            select_info& operator=(const select_info&) = delete;
        };
        typedef std::shared_ptr<select_info> select_info_sp;

        select_info_sp spinfo_;     // Shared information about elements.
        iterator_type icur_;        // Iterator pointing at current element.
        std::size_t idx_;           // Index of current element.

    public:
        next_impl(Seq&& seq, Selector&& sel)
            : spinfo_(std::make_shared<select_info>(std::forward<Seq>(seq),
                                                    std::forward<Selector>(sel))),
              icur_(std::begin(spinfo_->seq_)), idx_(0) { }

        auto operator()(cl::optional<U>& opt) -> const U* {
            const U* pobj = nullptr;
            if (icur_ != spinfo_->iend_) {
                opt = spinfo_->sel_(*icur_, idx_);
                pobj = std::addressof(*opt);
                ++icur_;
                ++idx_;
            }
            return pobj;
        }
    };

private:
    Selector sel_;  // Selector used to transform elements.
#ifdef _DEBUG
    bool applied_;  // Tracks operator application
#endif

public:
    explicit select_impl(Selector&& sel)
        : sel_(std::forward<Selector>(sel))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    template<typename Seq,
             typename _SelectorRes = decltype(std::declval<Selector>()(std::declval<typename seq_traits<Seq>::const_reference>(), std::declval<std::size_t>())),
             typename _U = typename coveo::detail::seq_element_traits<_SelectorRes>::raw_value_type>
    auto operator()(Seq&& seq) -> coveo::enumerable<_U> {
        // Note: if sel_ is not a ref, it will be moved by the forward
        // below and become invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        return next_impl<Seq, _SelectorRes, _U>(std::forward<Seq>(seq), std::forward<Selector>(sel_));
    }
};

// Implementation of select_many and select_many_with_index operators
template<typename Selector>
class select_many_impl
{
public:
    // Next delegate implementation for select_many operator
    template<typename Seq, typename U>
    class next_impl
    {
    private:
        // Iterator used by the sequence.
        typedef typename seq_traits<Seq>::iterator_type iterator_type;

        // Bean storing info about elements. Shared among delegates.
        struct select_info {
            Seq seq_;               // Sequence being transformed.
            iterator_type iend_;    // Iterator pointing at end of seq_.
            Selector sel_;          // Selector transforming the elements.

            select_info(Seq&& seq, Selector&& sel)
                : seq_(std::forward<Seq>(seq)),
                  iend_(std::end(seq_)),
                  sel_(std::forward<Selector>(sel)) { }

            // Cannot copy/move, stored in a shared_ptr
            select_info(const select_info&) = delete;
            select_info& operator=(const select_info&) = delete;
        };
        typedef std::shared_ptr<select_info> select_info_sp;

        select_info_sp spinfo_;     // Shared information about elements.
        iterator_type icur_;        // Iterator pointing at current element in sequence.
        std::size_t idx_;           // Index of current element in sequence.
        std::deque<U> cache_;       // Cache of results returned by selector.

    public:
        next_impl(Seq&& seq, Selector&& sel)
            : spinfo_(std::make_shared<select_info>(std::forward<Seq>(seq),
                                                    std::forward<Selector>(sel))),
              icur_(std::begin(spinfo_->seq_)), idx_(0), cache_() { }

        auto operator()(cl::optional<U>& opt) -> const U* {
            const U* pobj = nullptr;
            while (cache_.empty() && icur_ != spinfo_->iend_) {
                auto new_results = spinfo_->sel_(*icur_, idx_);
                cache_.insert(cache_.end(), std::begin(new_results), std::end(new_results));
                ++icur_;
                ++idx_;
            }
            if (!cache_.empty()) {
                opt = cache_.front();
                pobj = std::addressof(*opt);
                cache_.pop_front();
            }
            return pobj;
        }
    };

private:
    Selector sel_;  // Selector used to transform elements.
#ifdef _DEBUG
    bool applied_;  // Tracks operator application
#endif

public:
    explicit select_many_impl(Selector&& sel)
        : sel_(std::forward<Selector>(sel))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    template<typename Seq,
             typename _U = typename seq_traits<decltype(std::declval<Selector>()(std::declval<typename seq_traits<Seq>::const_reference>(), std::declval<std::size_t>()))>::raw_value_type>
    auto operator()(Seq&& seq) -> coveo::enumerable<_U> {
        // Note: if sel_ is not a ref, it will be moved by the forward
        // below and become invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        return next_impl<Seq, _U>(std::forward<Seq>(seq), std::forward<Selector>(sel_));
    }
};

// Implementation of skip/skip_while/skip_while_with_index operators
template<typename Pred>
class skip_impl
{
public:
    // Next delegate implementation for skip operators
    template<typename Seq>
    class next_impl
    {
    public:
        // Iterator for the sequence type.
        typedef typename seq_traits<Seq>::iterator_type iterator_type;

        // Bean containing info to share among delegates
        struct skip_info {
            Seq seq_;               // Sequence to skip elements from
            iterator_type iend_;    // Iterator pointing at end of sequence
            Pred pred_;             // Predicate to satisfy to skip elements

            skip_info(Seq&& seq, Pred&& pred)
                : seq_(std::forward<Seq>(seq)),
                  iend_(std::end(seq_)),
                  pred_(std::forward<Pred>(pred)) { }

            // Cannot copy/move, stored in a shared_ptr
            skip_info(const skip_info&) = delete;
            skip_info& operator=(const skip_info&) = delete;
        };
        typedef std::shared_ptr<skip_info> skip_info_sp;

    private:
        skip_info_sp spinfo_;   // Pointer to shared info
        iterator_type icur_;    // Iterator pointing at current element
        bool init_flag_;        // Whether icur_ has been initialized

    public:
        next_impl(Seq&& seq, Pred&& pred)
            : spinfo_(std::make_shared<skip_info>(std::forward<Seq>(seq),
                                                  std::forward<Pred>(pred))),
              icur_(), init_flag_(false) { }

        template<typename Op>
        auto operator()(Op&)
            -> typename seq_traits<Seq>::const_pointer
        {
            // Init starting point on first call
            if (!init_flag_) {
                icur_ = std::begin(spinfo_->seq_);
                std::size_t n = 0;
                while (icur_ != spinfo_->iend_ && spinfo_->pred_(*icur_, n++)) {
                    ++icur_;
                }
                init_flag_ = true;
            }
            typename seq_traits<Seq>::const_pointer pobj = nullptr;
            if (icur_ != spinfo_->iend_) {
                typename seq_traits<Seq>::const_reference robj = *icur_;
                pobj = std::addressof(robj);
                ++icur_;
            }
            return pobj;
        }
    };

private:
    Pred pred_;     // Predicate to satisfy to skip.
#ifdef _DEBUG
    bool applied_;  // Tracks operator application.
#endif

public:
    explicit skip_impl(Pred&& pred)
        : pred_(std::forward<Pred>(pred))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    template<typename Seq>
    auto operator()(Seq&& seq)
        -> coveo::enumerable<typename seq_traits<Seq>::raw_value_type>
    {
        // Note: if pred_ is not a ref, it will be moved by the forward
        // below and become invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        return next_impl<Seq>(std::forward<Seq>(seq), std::forward<Pred>(pred_));
    }
};

// Implementation of take/take_while/take_while_with_index operators
template<typename Pred>
class take_impl
{
public:
    // Next delegate implementation for take operators
    template<typename Seq>
    class next_impl
    {
    public:
        // Iterator for the sequence type.
        typedef typename seq_traits<Seq>::iterator_type iterator_type;

        // Bean containing info to share among delegates
        struct take_info {
            Seq seq_;               // Sequence to skip elements from
            iterator_type ibeg_;    // Iterator pointing at beginning of sequence
            iterator_type iend_;    // Iterator pointing at end of sequence
            Pred pred_;             // Predicate to satisfy to take elements

            take_info(Seq&& seq, Pred&& pred)
                : seq_(std::forward<Seq>(seq)),
                  ibeg_(std::begin(seq_)),
                  iend_(std::end(seq_)),
                  pred_(std::forward<Pred>(pred)) { }

            // Cannot copy/move, stored in a shared_ptr
            take_info(const take_info&) = delete;
            take_info& operator=(const take_info&) = delete;
        };
        typedef std::shared_ptr<take_info> take_info_sp;

    private:
        take_info_sp spinfo_;       // Pointer to shared info
        iterator_type icur_;        // Iterator pointing at current element
        iterator_type itake_end_;   // Iterator pointing after last element to take
        bool init_flag_;            // Whether icur_ and itake_end_ have been initialized

    public:
        next_impl(Seq&& seq, Pred&& pred)
            : spinfo_(std::make_shared<take_info>(std::forward<Seq>(seq),
                                                  std::forward<Pred>(pred))),
              icur_(), itake_end_(), init_flag_(false) { }

        template<typename Op>
        auto operator()(Op&)
            -> typename seq_traits<Seq>::const_pointer
        {
            // Init last point on first call
            if (!init_flag_) {
                icur_ = spinfo_->ibeg_;
                itake_end_ = icur_;
                std::size_t n = 0;
                while (itake_end_ != spinfo_->iend_ && spinfo_->pred_(*itake_end_, n++)) {
                    ++itake_end_;
                }
                init_flag_ = true;
            }
            typename seq_traits<Seq>::const_pointer pobj = nullptr;
            if (icur_ != itake_end_) {
                typename seq_traits<Seq>::const_reference robj = *icur_;
                pobj = std::addressof(robj);
                ++icur_;
            }
            return pobj;
        }
    };

private:
    Pred pred_;     // Predicate to satisfy to skip.
#ifdef _DEBUG
    bool applied_;  // Tracks operator application.
#endif

public:
    explicit take_impl(Pred&& pred)
        : pred_(std::forward<Pred>(pred))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    template<typename Seq>
    auto operator()(Seq&& seq)
        -> coveo::enumerable<typename seq_traits<Seq>::raw_value_type>
    {
        // Note: if pred_ is not a ref, it will be moved by the forward
        // below and become invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        return next_impl<Seq>(std::forward<Seq>(seq), std::forward<Pred>(pred_));
    }
};

// Implementation of union_with operator.
template<typename Seq2, typename Pred>
class union_impl
{
public:
    // Implementation of next delegate that filters duplicate elements
    template<typename Seq1>
    class next_impl
    {
    private:
        // Type of iterator for the sequences
        typedef typename seq_traits<Seq1>::iterator_type    first_iterator_type;
        typedef typename seq_traits<Seq2>::iterator_type    second_iterator_type;

        // Set storing seen elements
        typedef std::set<typename seq_traits<Seq1>::raw_value_type, proxy_cmp<Pred>>
                                                            seen_elements_set;

        // Info used to produce distinct elements. Shared among delegates.
        class union_info
        {
        private:
            Seq1 seq1_;                     // First sequence being iterated
            first_iterator_type iend1_;     // Iterator pointing at end of first sequence
            Seq2 seq2_;                     // Second sequence being iterated
            second_iterator_type iend2_;    // Iterator pointing at end of second sequence
            Pred pred_;                     // Predicate ordering the elements

        public:
            union_info(Seq1&& seq1, Seq2&& seq2, Pred&& pred)
                : seq1_(std::forward<Seq1>(seq1)),
                  iend1_(std::end(seq1_)),
                  seq2_(std::forward<Seq2>(seq2)),
                  iend2_(std::end(seq2_)),
                  pred_(std::forward<Pred>(pred)) { }

            // Cannot copy/move, stored in a shared_ptr
            union_info(const union_info&) = delete;
            union_info& operator=(const union_info&) = delete;

            first_iterator_type first_begin() {
                return std::begin(seq1_);
            }
            second_iterator_type second_begin() {
                return std::begin(seq2_);
            }
            seen_elements_set init_seen_elements() {
                return seen_elements_set(proxy_cmp<Pred>(pred_));
            }

            // Returns next distinct element in either sequence or nullptr when done
            auto get_next(first_iterator_type& icur1, second_iterator_type& icur2, seen_elements_set& seen)
                -> typename seq_traits<Seq1>::const_pointer
            {
                // First look for an element in first sequence
                typename seq_traits<Seq1>::const_pointer pobj = nullptr;
                for (; pobj == nullptr && icur1 != iend1_; ++icur1) {
                    typename seq_traits<Seq1>::const_reference robj = *icur1;
                    if (seen.emplace(robj).second) {
                        // Not seen yet, return this element.
                        pobj = std::addressof(robj);
                    }
                }

                // If we did not find an element in first sequence, try in second sequence
                for (; pobj == nullptr && icur2 != iend2_; ++icur2) {
                    typename seq_traits<Seq2>::const_reference robj = *icur2;
                    if (seen.emplace(robj).second) {
                        pobj = std::addressof(robj);
                    }
                }
                return pobj;
            }
        };
        typedef std::shared_ptr<union_info> union_info_sp;

        union_info_sp spinfo_;          // Shared info
        first_iterator_type icur1_;     // Iterator pointing at current element in first sequence
        second_iterator_type icur2_;    // Iterator pointing at current element in second sequence
        seen_elements_set seen_;        // Set of refs to seen elements

    public:
        next_impl(Seq1&& seq1, Seq2&& seq2, Pred&& pred)
            : spinfo_(std::make_shared<union_info>(std::forward<Seq1>(seq1),
                                                   std::forward<Seq2>(seq2),
                                                   std::forward<Pred>(pred))),
              icur1_(spinfo_->first_begin()),
              icur2_(spinfo_->second_begin()),
              seen_(spinfo_->init_seen_elements()) { }

        template<typename Op>
        auto operator()(Op&) {
            return spinfo_->get_next(icur1_, icur2_, seen_);
        }
    };

private:
    Seq2 seq2_;     // Second sequence to union.
    Pred pred_;     // Predicate used to compare elements
#ifdef _DEBUG
    bool applied_;  // Tracks operator application
#endif

public:
    explicit union_impl(Seq2&& seq2, Pred&& pred)
        : seq2_(std::forward<Seq2>(seq2)),
          pred_(std::forward<Pred>(pred))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    // Movable but not copyable
    union_impl(const union_impl&) = delete;
    union_impl(union_impl&&) = default;
    union_impl& operator=(const union_impl&) = delete;
    union_impl& operator=(union_impl&&) = default;

    template<typename Seq1>
    auto operator()(Seq1&& seq1)
        -> coveo::enumerable<typename seq_traits<Seq1>::raw_value_type>
    {
        // Note: if the members above are not refs, they are moved by the
        // forward below and become invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        return next_impl<Seq1>(std::forward<Seq1>(seq1),
                               std::forward<Seq2>(seq2_),
                               std::forward<Pred>(pred_));
    }
};

// Implementation of where/where_with_index operators.
template<typename Pred>
class where_impl
{
public:
    // Implementation of next delegate for where/where_with_index operators.
    template<typename Seq>
    class next_impl
    {
    public:
        // Iterator for the sequence.
        typedef typename seq_traits<Seq>::iterator_type iterator_type;

    private:
        // Bean storing info shared among delegates.
        struct where_info {
            Seq seq_;               // Sequence being iterated.
            iterator_type iend_;    // Iterator pointing at end of seq_
            Pred pred_;             // Predicate to satisfy

            where_info(Seq&& seq, Pred&& pred)
                : seq_(std::forward<Seq>(seq)),
                  iend_(std::end(seq_)),
                  pred_(std::forward<Pred>(pred)) { }

            // Cannot copy/move, stored in a shared_ptr
            where_info(const where_info&) = delete;
            where_info& operator=(const where_info&) = delete;
        };
        typedef std::shared_ptr<where_info> where_info_sp;

        where_info_sp spinfo_;      // Shared info containing sequence and predicate.
        iterator_type icur_;        // Pointer at current element.
        std::size_t idx_;           // Index of current element.

    public:
        next_impl(Seq&& seq, Pred&& pred)
            : spinfo_(std::make_shared<where_info>(std::forward<Seq>(seq),
                                                   std::forward<Pred>(pred))),
              icur_(std::begin(spinfo_->seq_)), idx_(0) { }

        template<typename Op>
        auto operator()(Op&) {
            typename seq_traits<Seq>::const_pointer pobj = nullptr;
            for (; pobj == nullptr && icur_ != spinfo_->iend_; ++icur_, ++idx_) {
                typename seq_traits<Seq>::const_reference robj = *icur_;
                if (spinfo_->pred_(robj, idx_)) {
                    // This element satistifies the predicate, return it.
                    pobj = std::addressof(robj);
                }
            }
            return pobj;
        }
    };

private:
    Pred pred_;     // Predicate to satisfy.
#ifdef _DEBUG
    bool applied_;  // Tracks operator application.
#endif

public:
    explicit where_impl(Pred&& pred)
        : pred_(std::forward<Pred>(pred))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    // Movable but not copyable
    where_impl(const where_impl&) = delete;
    where_impl(where_impl&&) = default;
    where_impl& operator=(const where_impl&) = delete;
    where_impl& operator=(where_impl&&) = default;

    template<typename Seq>
    auto operator()(Seq&& seq)
        -> coveo::enumerable<typename seq_traits<Seq>::raw_value_type>
    {
        // Note: if pred_ is not a ref, it will be moved by the forward
        // below and become invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        return next_impl<Seq>(std::forward<Seq>(seq), std::forward<Pred>(pred_));
    }
};

// Implementation of zip operator.
template<typename Seq2, typename ResultSelector>
class zip_impl
{
public:
    // Implementation of next delegate for this operator.
    template<typename Seq1, typename SelectorRes, typename U>
    class next_impl
    {
    public:
        // Iterator types for the sequences.
        typedef typename seq_traits<Seq1>::iterator_type    first_iterator_type;
        typedef typename seq_traits<Seq2>::iterator_type    second_iterator_type;

    private:
        // Bean storing info shared among delegates.
        struct zip_info {
            Seq1 seq1_;                     // First sequence to zip.
            first_iterator_type iend1_;     // Iterator pointing at end of seq1_.
            Seq2 seq2_;                     // Second sequence to zip.
            second_iterator_type iend2_;    // Iterator pointing at end of seq2_.
            ResultSelector result_sel_;     // Selector producing the results.

            zip_info(Seq1&& seq1, Seq2&& seq2, ResultSelector&& result_sel)
                : seq1_(std::forward<Seq1>(seq1)),
                  iend1_(std::end(seq1_)),
                  seq2_(std::forward<Seq2>(seq2)),
                  iend2_(std::end(seq2_)),
                  result_sel_(std::forward<ResultSelector>(result_sel)) { }

            // Cannot copy/move, stored in a shared_ptr
            zip_info(const zip_info&) = delete;
            zip_info& operator=(const zip_info&) = delete;
        };
        typedef std::shared_ptr<zip_info>   zip_info_sp;

        zip_info_sp spinfo_;            // Bean containing shared info.
        first_iterator_type icur1_;     // Iterator pointing at current item in first sequence.
        second_iterator_type icur2_;    // Iterator pointing at current item in second sequence.

    public:
        next_impl(Seq1&& seq1, Seq2&& seq2, ResultSelector&& result_sel)
            : spinfo_(std::make_shared<zip_info>(std::forward<Seq1>(seq1),
                                                 std::forward<Seq2>(seq2),
                                                 std::forward<ResultSelector>(result_sel))),
              icur1_(std::begin(spinfo_->seq1_)),
              icur2_(std::begin(spinfo_->seq2_)) { }

        auto operator()(cl::optional<U>& opt) -> const U* {
            const U* pobj = nullptr;
            if (icur1_ != spinfo_->iend1_ && icur2_ != spinfo_->iend2_) {
                opt = spinfo_->result_sel_(*icur1_, *icur2_);
                pobj = std::addressof(*opt);
                ++icur1_;
                ++icur2_;
            }
            return pobj;
        }
    };

private:
    Seq2 seq2_;                     // Second sequence to zip.
    ResultSelector result_sel_;     // Selector producing the results.
#ifdef _DEBUG
    bool applied_;                  // Tracks operator application.
#endif

public:
    zip_impl(Seq2&& seq2, ResultSelector&& result_sel)
        : seq2_(std::forward<Seq2>(seq2)),
          result_sel_(std::forward<ResultSelector>(result_sel))
#ifdef _DEBUG
        , applied_(false)
#endif
    { }

    // Movable but not copyable
    zip_impl(const zip_impl&) = delete;
    zip_impl(zip_impl&&) = default;
    zip_impl& operator=(const zip_impl&) = delete;
    zip_impl& operator=(zip_impl&&) = default;

    template<typename Seq1,
             typename _SelectorRes = decltype(std::declval<ResultSelector>()(std::declval<typename seq_traits<Seq1>::const_reference>(),
                                                                             std::declval<typename seq_traits<Seq2>::const_reference>())),
             typename _U = typename coveo::detail::seq_element_traits<_SelectorRes>::raw_value_type>
    auto operator()(Seq1&& seq1) -> coveo::enumerable<_U> {
        // Note: if the members above are not refs, they are moved by the
        // forward below and become invalid; this cannot be called twice.
#ifdef _DEBUG
        assert(!applied_);
        applied_ = true;
#endif
        return next_impl<Seq1, _SelectorRes, _U>(std::forward<Seq1>(seq1),
                                                 std::forward<Seq2>(seq2_),
                                                 std::forward<ResultSelector>(result_sel_));
    }
};

} // detail
} // linq
} // coveo

#endif // COVEO_LINQ_DETAIL_H