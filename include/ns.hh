#pragma once

#include "gc.hh"
#include "percpu.hh"

// name spaces
// XXX maybe use open hash table, no chain, better cache locality

#if SPINLOCK_DEBUG
#define NHASH 10
#else
#define NHASH 257
#endif

template<class K, class V>
class xelem : public rcu_freed {
 public:
  V val;
  K key;

  std::atomic<int> next_lock;
  std::atomic<xelem<K, V>*> next;

  int percore_c;
  std::atomic<xelem<K, V>*> percore_next;
  std::atomic<xelem<K, V>*>* percore_pprev;

  xelem(const K &k, const V &v)
    : rcu_freed("xelem"), val(v), key(k),
      next_lock(0), next(0),
      percore_next(0), percore_pprev(0) {}
  virtual void do_gc() {
    delete this;
  }

  NEW_DELETE_OPS(xelem)
};

// XXX maybe not cache align, because it takes too much space
template<class K, class V>
struct xbucket {
  std::atomic<xelem<K, V>*> volatile chain;
} ; // __attribute__((aligned (CACHELINE)));

template<class K, class V, u64 (*HF)(const K&)>
class xns : public rcu_freed {
 private:
  bool allowdup;
  std::atomic<u64> nextkey;
  xbucket<K, V> table[NHASH];
  std::atomic<xelem<K, V>*> percore[NCPU];
  spinlock percore_lock[NCPU];

 public:
  xns(bool dup) : rcu_freed("xns") {
    allowdup = dup;
    nextkey = 1;
    for (int i = 0; i < NHASH; i++)
      table[i].chain = 0;
    for (int i = 0; i < NCPU; i++) {
      percore[i] = nullptr;
      initlock(&percore_lock[i], "xns_lock", LOCKSTAT_NS);
    }
  }

  ~xns() {
    for (int i = 0; i < NHASH; i++)
      if (table[i].chain)
        panic("~xns: not empty");
  }

  virtual void do_gc() {
    delete this;
  }

  u64 allockey() {
    return nextkey++;
  }

  u64 h(const K &key) {
    return HF(key) % NHASH;
  }

  int insert(const K &key, const V &val) {
    auto e = new xelem<K, V>(key, val);
    if (!e)
      return -1;

    u64 i = h(key);
    scoped_gc_epoch gc;

    for (;;) {
      auto root = table[i].chain;
      if (!allowdup) {
        for (auto x = root.load(); x; x = x->next) {
          if (x->key == key) {
            gc_delayed(e);
            return -1;
          }
        }
      }

      e->next = root.load();
      if (cmpxch(&table[i].chain, e->next.load(), e)) {
        int c = myid();
        acquire(&percore_lock[c]);
        e->percore_c = c;
        e->percore_next = percore[c].load();
        if (percore[c])
          percore[c].load()->percore_pprev = &e->percore_next;
        e->percore_pprev = &percore[c];
        percore[c] = e;
        release(&percore_lock[c]);
        return 0;
      }
    }
  }

  V lookup(const K &key) {
    u64 i = h(key);

    scoped_gc_epoch gc;
    auto e = table[i].chain.load();

    while (e) {
      if (e->key == key)
        return e->val;
      e = e->next;
    }

    return 0;
  }

  bool remove(const K &key, const V *vp = 0) {
    u64 i = h(key);
    scoped_gc_epoch gc;

    for (;;) {
      std::atomic<int> fakelock(0);
      std::atomic<int> *pelock = &fakelock;
      auto pe = &table[i].chain;

      for (;;) {
        auto e = pe->load();
        if (!e)
          return false;

        if (e->key == key && (!vp || e->val == *vp)) {
          if (!cmpxch(&e->next_lock, 0, 1))
            break;
          if (!cmpxch(pelock, 0, 1)) {
            e->next_lock = 0;
            break;
          }

          if (!cmpxch(pe, e, e->next.load())) {
            *pelock = 0;
            e->next_lock = 0;
            break;
          }

          int c = e->percore_c;
          acquire(&percore_lock[c]);
          *e->percore_pprev = e->percore_next.load();
          if (e->percore_next)
            e->percore_next.load()->percore_pprev = e->percore_pprev;
          release(&percore_lock[c]);

          *pelock = 0;
          gc_delayed(e);
          return true;
        }

        pe = &e->next;
        pelock = &e->next_lock;
      }
    }
  }

  template<class CB>
  void enumerate(CB cb) {
    scoped_gc_epoch gc;
    int cpuoffset = myid();
    for (int i = 0; i < NCPU; i++) {
      auto e = percore[(i + cpuoffset) % NCPU].load();
      while (e) {
        if (cb(e->key, e->val))
          return;
        e = e->percore_next;
      }
    }
  }

  template<class CB>
  void enumerate_key(const K &key, CB cb) {
    scoped_gc_epoch gc;
    u64 i = h(key);
    auto e = table[i].chain;
    while (e) {
      if (e->key == key && cb(e->key, e->val))
        return;
      e = e->next;
    }
  }

  class iterator {
  private:
    xns<K, V, HF> *ns_;
    xelem<K, V> *chain_;
    int ndx_;

  public:
    iterator(xns<K, V, HF> *ns) {
      if (ns_)
        gc_begin_epoch();
      ns_ = ns;
      ndx_ = 0;
      chain_ = ns->table[ndx_++].chain;
      for (; chain_ == 0 && ndx_ < NHASH; ndx_++)
        chain_ = ns_->table[ndx_].chain;
    }

    iterator() {
      ns_ = 0;
      ndx_ = NHASH;
      chain_ = 0;
    }

    ~iterator() {
      if (ns_)
        gc_end_epoch();
    }

    bool operator!=(const iterator &other) const {
      return other.chain_ != this->chain_;
    }

    iterator& operator ++() {
      for (chain_ = chain_->next; chain_ == 0 && ndx_ < NHASH; ndx_++)
        chain_ = ns_->table[ndx_].chain;
      return *this;
    }

    V& operator *() {
      return chain_->val;
    }
  };

  iterator begin() {
    return iterator(this);
  }

  iterator end() {
    return iterator();
  }

  NEW_DELETE_OPS(xns)
};

template<class K, class V, u64 (*HF)(const K&)>
static inline
typename xns<K, V, HF>::iterator
begin(xns<K, V, HF> *&ns)
{
  return ns->begin();
}

template<class K, class V, u64 (*HF)(const K&)>
static inline
typename xns<K, V, HF>::iterator
end(xns<K, V, HF> *&ns)
{
  return ns->end();
}
