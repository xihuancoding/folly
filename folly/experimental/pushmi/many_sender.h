/*
 * Copyright 2018-present Facebook, Inc.
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
#pragma once

#include <folly/experimental/pushmi/executor.h>
#include <folly/experimental/pushmi/receiver.h>
#include <folly/experimental/pushmi/trampoline.h>
#include <type_traits>

namespace folly {
namespace pushmi {

template <class E, class... VN>
class any_many_sender {
  union data {
    void* pobj_ = nullptr;
    std::aligned_union_t<0, std::tuple<VN...>> buffer_;
  } data_{};
  template <class Wrapped>
  static constexpr bool insitu() {
    return sizeof(Wrapped) <= sizeof(data::buffer_) &&
        std::is_nothrow_move_constructible<Wrapped>::value;
  }
  struct vtable {
    static void s_op(data&, data*) {}
    static void s_submit(data&, any_receiver<E, VN...>) {}
    void (*op_)(data&, data*) = vtable::s_op;
    void (*submit_)(data&, any_receiver<E, VN...>) = vtable::s_submit;
  };
  static constexpr vtable const noop_{};
  vtable const* vptr_ = &noop_;
  template <class Wrapped>
  any_many_sender(Wrapped obj, std::false_type) : any_many_sender() {
    struct s {
      static void op(data& src, data* dst) {
        if (dst)
          dst->pobj_ = std::exchange(src.pobj_, nullptr);
        delete static_cast<Wrapped const*>(src.pobj_);
      }
      static void submit(data& src, any_receiver<E, VN...> out) {
        ::folly::pushmi::submit(
            *static_cast<Wrapped*>(src.pobj_), std::move(out));
      }
    };
    static const vtable vtbl{s::op, s::submit};
    data_.pobj_ = new Wrapped(std::move(obj));
    vptr_ = &vtbl;
  }
  template <class Wrapped>
  any_many_sender(Wrapped obj, std::true_type) noexcept : any_many_sender() {
    struct s {
      static void op(data& src, data* dst) {
        if (dst)
          new (&dst->buffer_)
              Wrapped(std::move(*static_cast<Wrapped*>((void*)&src.buffer_)));
        static_cast<Wrapped const*>((void*)&src.buffer_)->~Wrapped();
      }
      static void submit(data& src, any_receiver<E, VN...> out) {
        ::folly::pushmi::submit(
            *static_cast<Wrapped*>((void*)&src.buffer_), std::move(out));
      }
    };
    static const vtable vtbl{s::op, s::submit};
    new (&data_.buffer_) Wrapped(std::move(obj));
    vptr_ = &vtbl;
  }
  template <class T, class U = std::decay_t<T>>
  using wrapped_t =
      std::enable_if_t<!std::is_same<U, any_many_sender>::value, U>;

 public:
  using properties = property_set<is_sender<>, is_many<>>;

  any_many_sender() = default;
  any_many_sender(any_many_sender&& that) noexcept : any_many_sender() {
    that.vptr_->op_(that.data_, &data_);
    std::swap(that.vptr_, vptr_);
  }

  PUSHMI_TEMPLATE(class Wrapped)
  (requires SenderTo<wrapped_t<Wrapped>, any_receiver<E, VN...>> &&
      is_many_v<wrapped_t<Wrapped>>) //
      explicit any_many_sender(Wrapped obj) //
      noexcept(insitu<Wrapped>())
      : any_many_sender{std::move(obj), bool_<insitu<Wrapped>()>{}} {}
  ~any_many_sender() {
    vptr_->op_(data_, nullptr);
  }
  any_many_sender& operator=(any_many_sender&& that) noexcept {
    this->~any_many_sender();
    new ((void*)this) any_many_sender(std::move(that));
    return *this;
  }
  PUSHMI_TEMPLATE(class Out)
  (requires ReceiveError<Out, E>&& ReceiveValue<Out, VN...>) //
      void submit(Out&& out) {
    vptr_->submit_(data_, any_receiver<E, VN...>{(Out &&) out});
  }
};

// Class static definitions:
template <class E, class... VN>
constexpr typename any_many_sender<E, VN...>::vtable const
    any_many_sender<E, VN...>::noop_;

template <class SF>
class many_sender<SF> {
  SF sf_;

 public:
  using properties = property_set<is_sender<>, is_many<>>;

  constexpr many_sender() = default;
  constexpr explicit many_sender(SF sf) : sf_(std::move(sf)) {}

  PUSHMI_TEMPLATE(class Out)
  (requires PUSHMI_EXP(
      lazy::Receiver<Out> PUSHMI_AND lazy::Invocable<SF&, Out>)) //
      void submit(Out out) {
    sf_(std::move(out));
  }
};

template <PUSHMI_TYPE_CONSTRAINT(Sender) Data, class DSF>
class many_sender<Data, DSF> {
  Data data_;
  DSF sf_;

 public:
  using properties = property_set_insert_t<
      properties_t<Data>,
      property_set<is_sender<>, is_many<>>>;

  constexpr many_sender() = default;
  constexpr explicit many_sender(Data data) : data_(std::move(data)) {}
  constexpr many_sender(Data data, DSF sf)
      : data_(std::move(data)), sf_(std::move(sf)) {}

  PUSHMI_TEMPLATE(class Out)
  (requires PUSHMI_EXP(
      lazy::Receiver<Out> PUSHMI_AND lazy::Invocable<DSF&, Data&, Out>)) //
      void submit(Out out) & {
    sf_(data_, std::move(out));
  }
  PUSHMI_TEMPLATE(class Out)
  (requires PUSHMI_EXP(
      lazy::Receiver<Out> PUSHMI_AND lazy::Invocable<DSF&, Data&&, Out>)) //
      void submit(Out out) && {
    sf_(std::move(data_), std::move(out));
  }
};

template <>
class many_sender<> : public many_sender<ignoreSF> {
 public:
  many_sender() = default;
};

////////////////////////////////////////////////////////////////////////////////
// make_many_sender
PUSHMI_INLINE_VAR constexpr struct make_many_sender_fn {
  inline auto operator()() const {
    return many_sender<ignoreSF>{};
  }
  PUSHMI_TEMPLATE(class SF)
  (requires True<> PUSHMI_BROKEN_SUBSUMPTION(&&not Sender<SF>)) //
      auto
      operator()(SF sf) const {
    return many_sender<SF>{std::move(sf)};
  }
  PUSHMI_TEMPLATE(class Data)
  (requires True<>&& Sender<Data> && is_many_v<Data>) //
      auto
      operator()(Data d) const {
    return many_sender<Data, passDSF>{std::move(d)};
  }
  PUSHMI_TEMPLATE(class Data, class DSF)
  (requires Sender<Data> && is_many_v<Data>) //
      auto
      operator()(Data d, DSF sf) const {
    return many_sender<Data, DSF>{std::move(d), std::move(sf)};
  }
} const make_many_sender{};

////////////////////////////////////////////////////////////////////////////////
// deduction guides
#if __cpp_deduction_guides >= 201703
many_sender()->many_sender<ignoreSF>;

PUSHMI_TEMPLATE(class SF)
(requires True<> PUSHMI_BROKEN_SUBSUMPTION(&&not Sender<SF>)) //
    many_sender(SF)
        ->many_sender<SF>;

PUSHMI_TEMPLATE(class Data)
(requires True<>&& Sender<Data> && is_many_v<Data>) //
    many_sender(Data)
        ->many_sender<Data, passDSF>;

PUSHMI_TEMPLATE(class Data, class DSF)
(requires Sender<Data> && is_many_v<Data>) //
    many_sender(Data, DSF)
        ->many_sender<Data, DSF>;
#endif

template <>
struct construct_deduced<many_sender> : make_many_sender_fn {};

} // namespace pushmi
} // namespace folly
