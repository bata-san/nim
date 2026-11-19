#pragma once
// MPEG-1 video decoder for CRI USM movies.
//
// The game's movies are MPEG-1 elementary streams written by CRI's TMPGEnc-based encoder. Their user
// data carries "IDCPREC" = 3: intra DC coefficients use 11-bit precision (an MPEG-2 feature that plain
// MPEG-1 decoders do not know), so a stock decoder multiplies every intra DC by 8 and the picture turns
// into saturated blocks. This decoder honours the precision and decodes slices in parallel.
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nim {

struct Mpeg1Picture {
    uint32_t width = 0, height = 0;       // coded size from the sequence header
    uint32_t stride_y = 0, stride_c = 0;  // plane strides (macroblock aligned)
    const uint8_t* y = nullptr;
    const uint8_t* cb = nullptr;
    const uint8_t* cr = nullptr;
    char type = 'I';                      // I, P or B
};

class Mpeg1Decoder {
public:
    using Output = std::function<void(const Mpeg1Picture&)>;
    explicit Mpeg1Decoder(Output out);
    ~Mpeg1Decoder();
    Mpeg1Decoder(const Mpeg1Decoder&) = delete;
    Mpeg1Decoder& operator=(const Mpeg1Decoder&) = delete;

    // Appends elementary-stream bytes; complete pictures are decoded and emitted in display order.
    void feed(std::span<const uint8_t> es);
    // Decodes whatever is buffered and emits the held reference picture.
    void flush();
    // Forget references and buffered data (seek). The next sequence header / I picture restarts decoding.
    void reset();

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    int intra_dc_precision() const { return dc_precision_; }
    size_t slice_errors() const { return slice_errors_; }

private:
    struct Frame;
    struct Slice {
        size_t offset, size;  // in buf_, after the start code
        int row;              // 1-based vertical position
    };
    void process(bool final);
    void handle_unit(size_t begin, size_t end);
    void begin_picture(std::span<const uint8_t> header);
    void finish_picture();
    void decode_slice(const Slice& s, Frame& cur);
    void emit(const Frame& f);
    Frame* free_frame();

    Output out_;
    std::vector<uint8_t> buf_;
    size_t scan_ = 0;          // next byte to search for a start code
    size_t unit_ = SIZE_MAX;   // start of the unit being collected
    bool have_sequence_ = false;
    uint32_t width_ = 0, height_ = 0, mb_w_ = 0, mb_h_ = 0;
    int dc_precision_ = 0;
    uint8_t intra_q_[64]{}, non_intra_q_[64]{};

    // current picture
    bool picture_open_ = false;
    char pic_type_ = 0;
    bool full_pel_fwd_ = false, full_pel_bwd_ = false;
    int f_code_fwd_ = 1, f_code_bwd_ = 1;
    std::vector<Slice> slices_;

    std::vector<std::unique_ptr<Frame>> frames_;
    Frame* fwd_ = nullptr;  // older reference
    Frame* bwd_ = nullptr;  // newer reference (held for display until the next reference)
    bool bwd_pending_ = false;
    size_t slice_errors_ = 0;
};

// BT.601 limited-range YCbCr 4:2:0 (MPEG-1 centred chroma, bilinear) to RGBA, rows in parallel.
void mpeg1_to_rgba(const Mpeg1Picture& p, uint32_t w, uint32_t h, uint8_t* out);

} // namespace nim
