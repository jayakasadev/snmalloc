/**
 * Focused unit test for `RBTree::for_each`.
 *
 * Builds a tree with a handful of known elements via `insert_elem`, walks
 * it with `for_each`, and checks:
 *   - every inserted element is visited exactly once (order-independent);
 *   - no extra elements are visited;
 *   - the tree is completely unchanged afterwards -- same `find` results,
 *     same `is_empty()` state, and (as a stronger check than `is_empty()`
 *     alone) the same complete set of elements still present when walked
 *     again with a second `for_each` call.
 *
 * The `Rep` here mirrors `redblack.cc`'s test representation exactly
 * (same `NodeRef`/`node`/`Rep` shapes), since that file already
 * demonstrates the minimal `RBRep` implementation `RBTree` requires and
 * there is no reason for this test to invent a different one.
 */
#include "test/opt.h"
#include "test/setup.h"
#include "test/usage.h"

#include <algorithm>
#include <iostream>
#include <vector>

#ifndef SNMALLOC_TRACING
#  define SNMALLOC_TRACING
#endif
// Redblack tree needs some libraries with trace enabled.
#include "test/snmalloc_testlib.h"

#include <snmalloc/ds_core/redblacktree.h>

namespace
{
  struct NodeRef
  {
    // The redblack tree is going to be used inside the pagemap,
    // and the redblack tree cannot use all the bits.  Applying an offset
    // to the stored value ensures that we have some abstraction over
    // the representation.
    static constexpr size_t offset = 10000;

    size_t* ptr;

    constexpr NodeRef(size_t* p) : ptr(p) {}

    constexpr NodeRef() : ptr(nullptr) {}

    constexpr NodeRef(const NodeRef& other) : ptr(other.ptr) {}

    constexpr NodeRef(NodeRef&& other) : ptr(other.ptr) {}

    bool operator!=(const NodeRef& other) const
    {
      return ptr != other.ptr;
    }

    NodeRef& operator=(const NodeRef& other)
    {
      ptr = other.ptr;
      return *this;
    }

    void set(uint16_t val)
    {
      *ptr = ((size_t(val) + offset) << 1) + (*ptr & 1);
    }

    explicit operator uint16_t()
    {
      return uint16_t((*ptr >> 1) - offset);
    }

    explicit operator size_t*()
    {
      return ptr;
    }
  };

  // Simple representation that is like the pagemap.
  // Bottom bit of left is used to store the colour.
  // We shift the fields up to make room for the colour.
  struct node
  {
    size_t left;
    size_t right;
  };

  inline static node array[2048];

  class Rep
  {
  public:
    using key = uint16_t;

    static constexpr key null = 0;
    static constexpr size_t root{NodeRef::offset << 1};

    using Handle = NodeRef;
    using Contents = uint16_t;

    static void set(Handle ptr, Contents r)
    {
      ptr.set(r);
    }

    static Contents get(Handle ptr)
    {
      return static_cast<Contents>(ptr);
    }

    static Handle ref(bool direction, key k)
    {
      if (direction)
        return {&array[k].left};
      else
        return {&array[k].right};
    }

    static bool is_red(key k)
    {
      return (array[k].left & 1) == 1;
    }

    static void set_red(key k, bool new_is_red)
    {
      if (new_is_red != is_red(k))
        array[k].left ^= 1;
    }

    static bool compare(key k1, key k2)
    {
      return k1 > k2;
    }

    static bool equal(key k1, key k2)
    {
      return k1 == k2;
    }

    static size_t printable(key k)
    {
      return k;
    }

    static size_t* printable(NodeRef k)
    {
      return static_cast<size_t*>(k);
    }

    static const char* name()
    {
      return "TestRep";
    }
  };

  /**
   * Collect every value `for_each` visits into `out`, sorted, so callers
   * can compare against a sorted expected set without depending on
   * `for_each`'s (unspecified) traversal order.
   */
  void collect_sorted(
    snmalloc::RBTree<Rep, true>& tree, std::vector<Rep::key>& out)
  {
    out.clear();
    tree.for_each([&](Rep::key k) { out.push_back(k); });
    std::sort(out.begin(), out.end());
  }

  void fail(const char* msg)
  {
    std::cout << "FAILED: " << msg << std::endl;
    abort();
  }

  /**
   * Basic coverage: insert a handful of known elements, walk with
   * `for_each`, and check the visited set matches exactly -- no missing
   * elements, no duplicates, no phantom elements.
   */
  void test_visits_exactly_inserted_elements()
  {
    snmalloc::RBTree<Rep, true> tree;

    std::vector<Rep::key> inserted{7, 3, 19, 1, 42, 5, 23, 11};
    for (auto k : inserted)
    {
      if (!tree.insert_elem(k))
        fail("insert_elem unexpectedly reported a duplicate");
    }

    std::vector<Rep::key> visited;
    collect_sorted(tree, visited);

    std::vector<Rep::key> expected = inserted;
    std::sort(expected.begin(), expected.end());

    if (visited != expected)
      fail("for_each did not visit exactly the inserted elements");

    // Every inserted element must still be `find`-able -- for_each must
    // not have mutated the tree.
    for (auto k : inserted)
    {
      auto path = tree.get_root_path();
      if (!tree.find(path, k))
        fail("element no longer findable after for_each");
    }

    if (tree.is_empty())
      fail("tree unexpectedly empty after inserts + for_each");
  }

  /**
   * `for_each` on an empty tree must visit nothing and must not disturb
   * `is_empty()`.
   */
  void test_empty_tree()
  {
    snmalloc::RBTree<Rep, true> tree;

    size_t visits = 0;
    tree.for_each([&](Rep::key) { visits++; });

    if (visits != 0)
      fail("for_each visited nodes in an empty tree");

    if (!tree.is_empty())
      fail("empty tree not is_empty() after for_each");
  }

  /**
   * Calling `for_each` twice in a row must observe exactly the same set
   * both times -- the read-only walk must leave no residue that would
   * perturb a subsequent traversal (a stronger check than `is_empty()`
   * alone, since it also pins down that no nodes were silently dropped,
   * duplicated, or reordered into an inconsistent structure that a
   * second walk would expose differently).
   */
  void test_repeated_for_each_is_stable()
  {
    snmalloc::RBTree<Rep, true> tree;

    std::vector<Rep::key> inserted{100, 50, 150, 25, 75, 125, 175, 12, 200};
    for (auto k : inserted)
    {
      if (!tree.insert_elem(k))
        fail("insert_elem unexpectedly reported a duplicate");
    }

    std::vector<Rep::key> first;
    collect_sorted(tree, first);

    std::vector<Rep::key> second;
    collect_sorted(tree, second);

    if (first != second)
      fail("two consecutive for_each calls observed different node sets");

    std::vector<Rep::key> expected = inserted;
    std::sort(expected.begin(), expected.end());
    if (first != expected)
      fail("for_each result does not match the inserted set");

    // Tree must still behave normally afterwards: removing every element
    // via remove_elem must succeed and drain it to empty, exactly as it
    // would have without the for_each calls above.
    for (auto k : inserted)
    {
      if (!tree.remove_elem(k))
        fail("remove_elem failed after for_each traversal(s)");
    }

    if (!tree.is_empty())
      fail("tree not empty after removing every inserted element");
  }

  /**
   * Interleave `for_each` with tree mutation (not concurrently -- this
   * traversal API is documented as not safe to call while mutating, see
   * `redblacktree.h` -- but sequentially, one after another) across a
   * pseudo-random workload, to exercise `for_each` against a variety of
   * tree shapes beyond the small hand-picked cases above.  Reuses
   * `redblack.cc`'s xoroshiro generator convention for determinism.
   */
  void test_matches_reference_set_across_random_mutations(unsigned int seed)
  {
    // Deterministic but distinct per call: cheap linear congruential
    // sequence is more than sufficient for this smoke coverage, so we
    // don't need to pull in xoroshiro here just for a handful of calls.
    uint32_t state = seed;
    auto next = [&]() {
      state = state * 1664525u + 1013904223u;
      return state;
    };

    snmalloc::RBTree<Rep, true> tree;
    std::vector<Rep::key> reference;

    for (size_t round = 0; round < 200; round++)
    {
      auto op = next() % 3;
      if (op != 2 || reference.empty())
      {
        auto k = static_cast<Rep::key>(1 + (next() % 500));
        if (tree.insert_elem(k))
        {
          reference.push_back(k);
        }
      }
      else
      {
        auto idx = next() % reference.size();
        auto k = reference[idx];
        if (!tree.remove_elem(k))
          fail("remove_elem failed for an element the reference set has");
        reference.erase(reference.begin() + static_cast<long>(idx));
      }

      std::vector<Rep::key> visited;
      collect_sorted(tree, visited);

      auto expected = reference;
      std::sort(expected.begin(), expected.end());

      if (visited != expected)
        fail("for_each result diverged from the reference set");
    }
  }
}

int main(int argc, char** argv)
{
  setup();

  opt::Opt opt(argc, argv);
  snmalloc::UNUSED(opt);

  test_empty_tree();
  test_visits_exactly_inserted_elements();
  test_repeated_for_each_is_stable();
  test_matches_reference_set_across_random_mutations(1);
  test_matches_reference_set_across_random_mutations(42);

  std::cout << "redblack_for_each: all tests passed" << std::endl;
  return 0;
}
