#pragma once
#include "../Map.h"
#ifdef NICE_GFX_UI_HEADER
#include NICE_GFX_UI_HEADER
#else
#include <UI/View.h>
#endif
#include <new>
#include <array>
#include <atomic>
#include <cstring>
#include <utility>

namespace nice_gfx_map {
namespace detail {
inline void releaseUiImage(void *context, const void *) { delete static_cast<TileImage *>(context); }
inline void swapUiPixelBytes(uint16_t *pixels, size_t count) {
  // Custom TileImage allocators guarantee halfword alignment, not word
  // alignment. Peel that prefix before asking the compiler for word accesses.
  while(count && reinterpret_cast<uintptr_t>(pixels)%alignof(uint32_t)) {
    *pixels=uint16_t((*pixels<<8)|(*pixels>>8)); ++pixels; --count;
  }
  if(!count)return;
  auto *bytes=reinterpret_cast<unsigned char *>(pixels);
#if defined(__GNUC__) || defined(__clang__)
  bytes=static_cast<unsigned char *>(__builtin_assume_aligned(bytes,alignof(uint32_t)));
#endif
  for(;count>=2;count-=2,bytes+=sizeof(uint32_t)) {
    uint32_t pair;
    // memcpy preserves the uint16_t objects' aliasing rules. The known word
    // alignment lets ESP32 emit one 32-bit load/store for two PSRAM pixels.
    std::memcpy(&pair,bytes,sizeof(pair));
#if defined(__GNUC__) || defined(__clang__)
    pair=__builtin_bswap32((pair<<16)|(pair>>16));
#else
    pair=((pair&0x00ff00ffu)<<8)|((pair>>8)&0x00ff00ffu);
#endif
    std::memcpy(bytes,&pair,sizeof(pair));
  }
  if(count) {
    uint16_t last;
    std::memcpy(&last,bytes,sizeof(last));
    last=uint16_t((last<<8)|(last>>8));
    std::memcpy(bytes,&last,sizeof(last));
  }
}
}
// Full-viewport bridge for an existing dynamic gfx-media node. All calls must
// run in the View owner task. Native Map nodes/editor ABI are not changed.
// Decorators may synchronously annotate the owned image before it becomes
// immutable. They must not retain/move it or mutate Map/View during this call.
// publish() decorators see native colors and retain the legacy final Wire
// conversion. publishWire() decorators see high-byte-first pixels;
// publishNative() decorators and retained Images use native RGB565 throughout.
using UiImageDecorator = void (*)(TileImage &image, void *context);

// Owner-task cumulative timings, including time spent in failed attempts.
// A null profiling clock leaves every counter unchanged and is never called.
struct UiPublishStats {
  uint64_t allocateMicros=0,renderMicros=0,decorateMicros=0,adoptMicros=0;
};
struct UiImagePoolStats {size_t allocatedSlots=0,borrowedSlots=0,bytes=0;};

// Three bounded pixel banks retain allocations across publications. A slot is
// reused only after View states, raster snapshots and route caches release its
// immutable Image. Leases keep the bank alive even if this publisher is gone.
class UiImagePublisher {
  struct Slot { TileImage image; std::atomic<bool> borrowed{false}; };
  struct Bank { std::array<Slot,3> slots; std::atomic<size_t> refs{1}; };
  struct Lease { Bank *bank; size_t index; };
  Bank *bank_=nullptr;
  nice_gfx::htmlui::Image latest_;
  size_t latestSlot_=0;
  bool latestNative_=false;
  bool pixelAllocationFailed_=false;
  uint32_t (*profilingClock_)()=nullptr;
  UiPublishStats profile_;
  struct ProfileStage {
    uint32_t (*clock)();
    uint64_t &total;
    uint32_t started;
    ProfileStage(uint32_t (*source)(),uint64_t &counter):clock(source),total(counter),started(source?source():0) {}
    ~ProfileStage(){if(clock)total+=uint32_t(clock()-started);}
  };
  static void drop(Bank *bank) {
    if(bank && bank->refs.fetch_sub(1,std::memory_order_acq_rel)==1) delete bank;
  }
  static void release(void *context,const void *) {
    auto *lease=static_cast<Lease *>(context);
    // Image's final reference may be dropped by a task other than the View
    // owner. Publish sees this slot only after that immutable lease is gone.
    lease->bank->slots[lease->index].borrowed.store(false,std::memory_order_release);
    drop(lease->bank);delete lease;
  }
 public:
  UiImagePublisher()=default;
  ~UiImagePublisher(){drop(bank_);}
  UiImagePublisher(const UiImagePublisher &)=delete;
  UiImagePublisher &operator=(const UiImagePublisher &)=delete;
  void setProfilingClock(uint32_t (*clock)()){profilingClock_=clock;}
  UiPublishStats profilingStats()const{return profile_;}
  bool lastPixelAllocationFailed()const{return pixelAllocationFailed_;}
  UiImagePoolStats poolStats()const {
    UiImagePoolStats stats;
    if(bank_)for(const auto &slot:bank_->slots){
      if(slot.image.valid()){++stats.allocatedSlots;stats.bytes+=slot.image.bytes();}
      if(slot.borrowed.load(std::memory_order_acquire))++stats.borrowedSlots;
    }
    return stats;
  }
  bool matchesUi(const nice_gfx::htmlui::View &ui,nice_gfx::htmlui::Id id) const {
    return ui.hasImage(id,latest_);
  }
  // Copy complete Native rows from our latest successful immutable image into
  // a separate caller-owned image. No allocation or extra lease is retained.
  // A Wire publication or any source/target alias rejects before writing.
  bool copyLatestNativeRows(TileImage &destination,uint16_t firstRow,uint16_t rowCount)const {
    if(!bank_||!latestNative_||!latest_.valid()||latestSlot_>=bank_->slots.size())return false;
    const auto &slot=bank_->slots[latestSlot_];
    const auto &source=slot.image;
    if(!slot.borrowed.load(std::memory_order_acquire)||!source.valid()||source.alpha()||
       !destination.valid()||destination.alpha()||source.width()!=destination.width()||
       source.height()!=destination.height()||!rowCount||firstRow>=source.height()||
       rowCount>source.height()-firstRow)return false;
    const auto view=latest_.view();
    const size_t strideBytes=size_t(source.width())*sizeof(uint16_t);
    if(view.pixels!=source.pixels()||view.width!=source.width()||view.height!=source.height()||
       view.format!=nice_gfx::htmlui::ImageFormat::RGB565||view.order!=nice_gfx::PixelOrder::Native||
       view.strideBytes!=strideBytes||view.byteLength!=source.bytes())return false;
    const size_t offset=size_t(firstRow)*strideBytes,bytes=size_t(rowCount)*strideBytes;
    if(offset>source.bytes()||bytes>source.bytes()-offset||
       offset>destination.bytes()||bytes>destination.bytes()-offset)return false;
    const uintptr_t input=reinterpret_cast<uintptr_t>(source.pixels());
    const uintptr_t output=reinterpret_cast<uintptr_t>(destination.pixels());
    if(input>UINTPTR_MAX-source.bytes()||output>UINTPTR_MAX-destination.bytes()||
       (input<output+destination.bytes()&&output<input+source.bytes()))return false;
    std::memmove(reinterpret_cast<uint8_t *>(destination.pixels())+offset,
                 reinterpret_cast<const uint8_t *>(source.pixels())+offset,bytes);
    return true;
  }
  // View owner task only, like publish(). Release spare pixel storage after an
  // inactive page's snapshots retire, without touching any immutable lease or
  // latest_ identity. A later publication reallocates only the selected slot.
  size_t trimUnused() {
    if(!bank_)return 0;
    size_t bytes=0;
    for(auto &slot:bank_->slots)if(!slot.borrowed.load(std::memory_order_acquire)){
      bytes+=slot.image.bytes();slot.image.reset();
    }
    return bytes;
  }
  Status publish(const Map &map,nice_gfx::htmlui::View &ui,nice_gfx::htmlui::Id id,
                 Allocator allocator={},UiImageDecorator decorate=nullptr,void *context=nullptr) {
    return decorate?publishImpl<false>(map,ui,id,allocator,decorate,context):
      publishImpl<true>(map,ui,id,allocator,nullptr,context);
  }
  Status publishWire(const Map &map,nice_gfx::htmlui::View &ui,nice_gfx::htmlui::Id id,
                 Allocator allocator={},UiImageDecorator decorate=nullptr,void *context=nullptr) {
    return publishImpl<true>(map,ui,id,allocator,decorate,context);
  }
  Status publishNative(const Map &map,nice_gfx::htmlui::View &ui,nice_gfx::htmlui::Id id,
                 Allocator allocator={},UiImageDecorator decorate=nullptr,void *context=nullptr,
                 RasterBlitter blitter={}) {
    return publishImpl<false,true>(map,ui,id,allocator,decorate,context,blitter);
  }
 private:
  template<bool Wire,bool NativeOutput=false>
  Status publishImpl(const Map &map,nice_gfx::htmlui::View &ui,nice_gfx::htmlui::Id id,
                 Allocator allocator,UiImageDecorator decorate,void *context,RasterBlitter blitter={}) {
    pixelAllocationFailed_=false;
    size_t selected=0;
    {
      ProfileStage timer(profilingClock_,profile_.allocateMicros);
      if(!map.width()||!map.height()||map.width()>1024||map.height()>1024||
         bool(allocator.allocate)!=bool(allocator.release))return Status::InvalidArgument;
      if(!bank_)bank_=new(std::nothrow) Bank;
      if(!bank_)return Status::NoMemory;
      selected=bank_->slots.size();
      for(size_t i=0;i<bank_->slots.size();++i)
        if(!bank_->slots[i].borrowed.load(std::memory_order_acquire)&&bank_->slots[i].image.valid()&&
           bank_->slots[i].image.width()==map.width()&&bank_->slots[i].image.height()==map.height()){selected=i;break;}
      if(selected==bank_->slots.size())for(size_t i=0;i<bank_->slots.size();++i)
        if(!bank_->slots[i].borrowed.load(std::memory_order_acquire)){selected=i;break;}
      if(selected==bank_->slots.size())return Status::Busy;
      auto &slot=bank_->slots[selected];
      if(slot.image.width()!=map.width()||slot.image.height()!=map.height()){
        const Status allocated=slot.image.allocate(map.width(),map.height(),false,allocator);
        if(allocated!=Status::Ok){pixelAllocationFailed_=allocated==Status::NoMemory;return allocated;}
      }
    }
    auto &slot=bank_->slots[selected];
    Status result;
    {
      ProfileStage timer(profilingClock_,profile_.renderMicros);
      if constexpr (Wire) result=map.renderRegionWire(0,0,map.width(),map.height(),slot.image.pixels(),slot.image.bytes()/2);
      else if constexpr (NativeOutput) result=map.renderRegionWithBlitter(0,0,map.width(),map.height(),slot.image.pixels(),slot.image.bytes()/2,0,blitter);
      else result=map.renderRegion(0,0,map.width(),map.height(),slot.image.pixels(),slot.image.bytes()/2);
    }
    if(result!=Status::Ok)return result;
    if(decorate){ProfileStage timer(profilingClock_,profile_.decorateMicros);decorate(slot.image,context);}
    ProfileStage timer(profilingClock_,profile_.adoptMicros);
    // Wire rasterization already produced the exact immutable UI bytes. Legacy
    // native decorators retain their contract and use the existing final swap.
    auto *pixels=slot.image.pixels();
    if constexpr (!Wire&&!NativeOutput) detail::swapUiPixelBytes(pixels,size_t(map.width())*map.height());
    auto *lease=new(std::nothrow) Lease{bank_,selected};
    if(!lease)return Status::NoMemory;
    slot.borrowed.store(true,std::memory_order_relaxed);
    bank_->refs.fetch_add(1,std::memory_order_relaxed);
    nice_gfx::htmlui::Image image;
    const auto adopted=nice_gfx::htmlui::Image::adopt(
      nice_gfx::htmlui::ImageView(pixels,map.width(),map.height(),slot.image.bytes(),
        nice_gfx::htmlui::ImageFormat::RGB565,NativeOutput?nice_gfx::PixelOrder::Native:nice_gfx::PixelOrder::Wire),release,lease,image);
    if(adopted!=nice_gfx::Status::Ok){release(lease,nullptr);return Status::NoMemory;}
    const auto native=ui.setImage(id,image);
    if(native==nice_gfx::Status::Ok){latest_=image;latestSlot_=selected;latestNative_=NativeOutput;}
    return native==nice_gfx::Status::Ok?Status::Ok:native==nice_gfx::Status::NoMemory?Status::NoMemory:Status::InvalidArgument;
  }
};
inline Status publishToUi(const Map &map, nice_gfx::htmlui::View &ui,
                          nice_gfx::htmlui::Id id, Allocator allocator = {},
                          UiImageDecorator decorate = nullptr, void *context = nullptr) {
  if (!map.width() || !map.height() || map.width() > 1024 || map.height() > 1024)
    return Status::InvalidArgument;
  std::unique_ptr<TileImage> image(new (std::nothrow) TileImage);
  if (!image) return Status::NoMemory;
  Status status = image->allocate(map.width(), map.height(), false, allocator);
  if (status != Status::Ok) return status;
  status = map.renderRegion(0, 0, map.width(), map.height(), image->pixels(), image->bytes() / 2);
  if (status != Status::Ok) return status;
  if (decorate) decorate(*image, context);
  nice_gfx::htmlui::Image shared;
  auto native = nice_gfx::htmlui::Image::adopt(
    nice_gfx::htmlui::ImageView(image->pixels(), map.width(), map.height(), image->bytes()),
    detail::releaseUiImage, image.get(), shared);
  if (native != nice_gfx::Status::Ok)
    return native == nice_gfx::Status::NoMemory ? Status::NoMemory : Status::InvalidArgument;
  image.release(); // shared now owns the immutable pixels, including in-flight frames.
  native = ui.setImage(id, shared);
  if (native == nice_gfx::Status::Ok) return Status::Ok;
  return native == nice_gfx::Status::NoMemory ? Status::NoMemory : Status::InvalidArgument;
}
} // namespace nice_gfx_map
