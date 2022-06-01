/*
quick and dirty implementation of gif decoder

Resources:
- https://en.wikipedia.org/wiki/GIF
- https://www.w3.org/Graphics/GIF/spec-gif89a.txt
- http://www.matthewflickinger.com/lab/whatsinagif/
- http://www.daubnet.com/en/file-format-gif
- https://www.cs.albany.edu/~sdc/csi333/Fal07/Lect/L18/Summary

to compile: g++ gif_decode.cpp -o gif_decode -std=c++14 -lSDL2

TODO:
- fix lzw: resulting index stream size is smaller than expected? try newton's cradle gif and rick roll gif
- better data structure for lzw table
- use a state machine for decoding process?
*/

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <bitset>
#include <string>
#include <sstream>
#include <memory>

#include <cassert>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

typedef enum BlockType
{
    BT_IMAGE = 0,
    BT_GRAPHIC_CONTROL,
    BT_APPLICATION_EXTENSION,
    BT_COMMENT_BLOCK
} BlockType;

struct Color
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

/* Base class for a "meaningful" block for GIF */
class GIFBlock
{
public:
    GIFBlock() = default;
    GIFBlock(BlockType type_) : type(type_) {}
    virtual ~GIFBlock() = default;

    BlockType type;
};

class Image : public GIFBlock
{
public:
    Image() : GIFBlock(BT_IMAGE) {}

    size_t width;
    size_t height;
    int left;
    int top;
    bool interlace;

    std::vector<Color> ct;
    std::vector<int> index;
};

class GraphicsControl : public GIFBlock
{
public:
    GraphicsControl() : GIFBlock(BT_GRAPHIC_CONTROL) {}

    bool transparent;
    bool user_input;
    uint8_t disposal_method;
    int delay_time;
    uint8_t color_index; /* for transparency */
};

class ApplicationExtension : public GIFBlock
{
public:
    ApplicationExtension() : GIFBlock(BT_APPLICATION_EXTENSION) {}

    char appid[8];
    int8_t authcode[3];

    std::vector<std::vector<int8_t>> data_blocks;
};

class CommentBlock : public GIFBlock
{
public:
    CommentBlock() : GIFBlock(BT_COMMENT_BLOCK) {}

    std::vector<std::string> comments;
};

const std::string block_type_str[4] = {
    "IMAGE",
    "GRAPHIC CONTROL",
    "APPLICATION EXTENSION",
    "COMMENT EXTENSION"
};

const std::string disposal_method_str[4] = {
    "disposal method not specified",
    "do not dispose of graphic",
    "overwrite graphic with background color",
    "overwrite graphic with previous graphic"
};

inline bool get_bit(int8_t n, int p)
{
    return (n & (1 << (p))) != 0;
}

/* retrieve value from n starting from bit position p with length l (e.g. ge_val(0b10010001, 4, 4) = 0b1001) */
inline int8_t get_val(int8_t n, int p, int l)
{
    return (n >> p) & ((1 << l) - 1);
}

template<typename T>
std::string HexToString(T uval)
{
    std::stringstream ss;
    ss << "0x" << std::setw(sizeof(uval) * 2) << std::setfill('0') << std::hex << +uval;
    return ss.str();
}

int fetch_code(const std::string& stream, int& pos, int code_size)
{
    pos -= code_size;
    return std::stoi(stream.substr(pos, code_size), nullptr, 2);
}

std::unordered_map<int, std::vector<int>> init_table(int ncolors, int code_size, int clear_code, int eoi_code)
{
    std::unordered_map<int, std::vector<int>> table;

    for (int i = 0; i < ncolors; ++i)
    {
        table[i] = std::vector<int>({i});
    }

    // the program will crush if i dont add these lines. idk
    table[clear_code] = std::vector<int>();
    table[eoi_code] = std::vector<int>();

    return table;
}

std::string DebugVector(const std::vector<int>& v)
{
    if (v.size() == 0)
    {
        return "{}";
    }

    std::string s = "{";

    for (int i = 0; i < v.size(); ++i)
    {
        s += std::to_string(int(v[i])) + ((i == v.size() - 1) ? "}" : ", ");
    }

    return s;
}

inline uint32_t color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

int main(int argc, char *argv[])
{
    if (argc <= 1)
    {
        std::cerr << "Usage: gif_decoder [FILE NAME].gif" << std::endl;
        return 1;
    }

    std::ifstream gif(argv[1]);
    std::vector<uint8_t> bytes;

    gif.seekg(0, gif.end);
    size_t length = gif.tellg();
    gif.seekg(0, gif.beg);

    if (length > 0)
    {
        bytes.resize(length);
        gif.read(reinterpret_cast<char*>(bytes.data()), length);
    }

    /* Process header block - bytes 0 to 5 */

    bool bGIF89a = bytes[4] == 0x39;
/*
    std::cerr << "HEADER BLOCK" << std::endl;
    std::cerr << bytes[0] << " - " << HexToString(bytes[0]) << std::endl; // G
    std::cerr << bytes[1] << " - " << HexToString(bytes[1]) << std::endl; // I
    std::cerr << bytes[2] << " - " << HexToString(bytes[2]) << std::endl; // F
    std::cerr << bytes[3] << " - " << HexToString(bytes[3]) << std::endl; // 8
    std::cerr << bytes[4] << " - " << HexToString(bytes[4]) << std::endl; // 7 or 9
    std::cerr << bytes[5] << " - " << HexToString(bytes[5]) << std::endl; // a
    std::cerr << std::endl;
*/
    assert(bGIF89a); /// support version 89a for now

    /* Process logical screen descriptor bytes 6 to 12 */

    size_t canvas_width = bytes[6] | (bytes[7] << 8); // data are stored in little-endian format
    size_t canvas_height = bytes[8] | (bytes[9] << 8);

    int8_t packed_field = bytes[10];

    bool gct_flag = get_bit(packed_field, 7); // most significant bit
    uint8_t N = get_val(packed_field, 4, 3); // color resolution
    int bkgd_color_idx = bytes[11];
/*
    std::cerr << "LOGICAL SCREEN DESCRIPTOR" << std::endl;
    std::cerr << "Canvas width: " << canvas_width << std::endl;
    std::cerr << "Canvas height: " << canvas_height << std::endl;
    std::cerr << "Global color table flag: " << gct_flag << std::endl;
    std::cerr << "Color resolution: " << int(N) << std::endl;
    std::cerr << "Background color index: " << bkgd_color_idx << std::endl;
    std::cerr << std::endl;
*/
    int idx = 13;

    std::vector<Color> gct;
    size_t ncolors = 1 << (N + 1);

    /* Process global color table (optional) */

    if (gct_flag)
    {
        //std::cerr << "GLOBAL COLOR TABLE" << std::endl;

        for (int i = 0; i < ncolors; ++i)
        {
            Color color;

            color.r = bytes[idx++];
            color.g = bytes[idx++];
            color.b = bytes[idx++];

            gct.push_back(color);

            //std::cerr << "Red: " << int(gct[i].r) << ", Green: " << int(gct[i].g) << ", Blue: " << int(gct[i].b) << std::endl;
        }

        //std::cerr << std::endl;
    }

    /* Process graphics control extension, application extension, comment extension, etc. until eof */

    std::vector<std::unique_ptr<GIFBlock>> blocks;

    bool done = false;

    /* http://giflib.sourceforge.net/whatsinagif/gif_file_stream.gif */
    while (!done)
    {
        uint8_t b = bytes[idx++];

        switch (b)
        {
        case 0x2C: // Image descriptor
        {
            Image i;

            i.left = bytes[idx++] | (bytes[idx++] << 8);
            i.top = bytes[idx++] | (bytes[idx++] << 8);
            i.width = bytes[idx++] | (bytes[idx++] << 8);
            i.height = bytes[idx++] | (bytes[idx++] << 8);

            int8_t packed_field = bytes[idx++];

            i.interlace = get_bit(packed_field, 6);

            bool lct_flag = get_bit(packed_field, 7); // most significant bit
            size_t lct_size = get_val(packed_field, 0, 3);
/*
            std::cerr << "IMAGE DESCRIPTOR" << std::endl;
            std::cerr << "Left: " << i.left << std::endl;
            std::cerr << "Top: " << i.top << std::endl;
            std::cerr << "Width: " << i.width << std::endl;
            std::cerr << "Height: " << i.height << std::endl;
            std::cerr << "Interlaced: " << i.interlace << std::endl;
            std::cerr << "Local color table flag: " << lct_flag << std::endl;
            std::cerr << std::endl;
*/
            /* Process local color table (optional) */

            if (lct_flag)
            {
                size_t ncolors = 1 << (lct_size + 1);
                std::vector<Color> lct;

                //std::cerr << "LOCAL COLOR TABLE" << std::endl;

                for (int i = 0; i < ncolors; ++i)
                {
                    Color color;

                    color.r = bytes[idx++];
                    color.g = bytes[idx++];
                    color.b = bytes[idx++];

                    lct.push_back(color);

                    //std::cerr << "Red: " << int(lct[i].r) << ", Green: " << int(lct[i].g) << ", Blue: " << int(lct[i].b) << std::endl;
                }

                //std::cerr << std::endl;

                i.ct = lct;
            }
            else
            {
                i.ct = gct; // use global color table instead
            }

            /* Fun part begins here */

            size_t lzw_min = bytes[idx++]; // minimum number of bits to represent a color (or pixel)
            size_t ncolors = i.ct.size();

            // Before decoding, let's prepare a stream of bytes (in binary)

            std::string stream = "";

            while (true)
            {
                size_t nbytes = bytes[idx++]; // size of a data sub-block

                if (nbytes == 0)
                {
                    break;
                }

                for (int i = 0; i < nbytes; ++i)
                {
                    stream = std::bitset<8>(bytes[idx++]).to_string() + stream;
                }
            }

            int first_code_size = lzw_min + 1; // number of bits needed for first code; 
            int code_size = first_code_size; // can be changed when necessary

            int clear_code = 1 << lzw_min;
            int eoi_code = clear_code + 1;

            std::unordered_map<int, std::vector<int>> table = init_table(ncolors, first_code_size, clear_code, eoi_code);
            std::vector<int> index_stream; // output we want

            int pos = stream.length();
            int code = fetch_code(stream, pos, code_size); // the first code should be the clear code and we just cleared so no need to do again
            int prev; // old code
            int table_index = eoi_code + 1; // counter for adding new entries to the table

            code = fetch_code(stream, pos, code_size); // our first color code
            index_stream.push_back(code);
            prev = code;

            int step = 1;
            //std::cerr << "LZW DECODING" << std::endl;

            while (true)
            {
                code = fetch_code(stream, pos, code_size);

                //std::cerr << "Step " << step << ": code=0b" << std::bitset<12>(code) << std::endl;

                if (code == clear_code)
                {
                    //std::cerr << "Reached CLEAR code" << std::endl;

                    code_size = first_code_size;
                    table = init_table(ncolors, code_size, clear_code, eoi_code);
                    table_index = eoi_code + 1;

                    code = fetch_code(stream, pos, code_size); // again, our first color code
                    index_stream.push_back(code);
                    prev = code;

                    continue;
                }
                else if (code == eoi_code)
                {
                    //std::cerr << "Reached EOI code" << std::endl;
                    break;
                }

                if (table.size() >= 4096)
                {
                    //continue; // till CC or EOI
                }

                if (table.find(code) == table.end())
                {
                    table[code] = table[prev];
                    table[code].push_back(table[prev][0]); // {CODE-1}+k
                    index_stream.insert(index_stream.end(), table[code].begin(), table[code].end());
                    table_index = code;
                }
                else // code exists in the table
                {
                    index_stream.insert(index_stream.end(), table[code].begin(), table[code].end());
                    table[table_index] = table[prev];
                    table[table_index].push_back(table[code][0]); // {CODE-1}+k
                }

                if (table.size() == (1 << code_size) && code_size < 12)
                {
                    code_size++; // increase as soon as the index is equal to 2^(code_size)-1
                }

                table_index++;
                prev = code;

                //std::cerr << "Step " << step << " result: index=" << DebugVector(index_stream) << std::endl;
                step++;
            }
/*
            std::cerr << std::endl;

            std::cerr << "LZW CODE TABLE" << std::endl;

            for (int i = 0; i < table_index; ++i)
            {
                std::cerr << "#" << i << ": " << DebugVector(table[i]) << std::endl;
            }
*/
            //std::cerr << std::endl;

            i.index = index_stream;
            assert(i.index.size() == i.width * i.height);
            blocks.push_back(std::make_unique<Image>(i));

            break;
        }
        case 0x21: // Extension introducer
        {
            uint8_t label = bytes[idx++];

            switch (label)
            {
            case 0xF9: // Graphic control extension (optional)
            {
                GraphicsControl gc;

                size_t block_size = bytes[idx++]; // always 4

                int8_t packed = bytes[idx++];

                gc.transparent = get_bit(packed, 0);
                gc.user_input = get_bit(packed, 1);
                gc.disposal_method = get_val(packed, 2, 3);

                gc.delay_time = bytes[idx++] | (bytes[idx++] << 8);
                gc.color_index = bytes[idx++];

                idx++; // skip the terminator

                blocks.push_back(std::make_unique<GraphicsControl>(gc));
/*
                std::cerr << "GRAPHIC CONTROL EXTENSION" << std::endl;
                std::cerr << "Is transparent: " << gc.transparent << std::endl;
                std::cerr << "User input enabled: " << gc.user_input << std::endl;
                std::cerr << "Disposal method: " << disposal_method_str[gc.disposal_method] << std::endl;
                std::cerr << "Delay time: " << gc.delay_time << std::endl;
                std::cerr << "Transparent color index: " << int(gc.color_index) << std::endl;
                std::cerr << std::endl;
*/
                break;
            }
            case 0xFF: // Application extension
            {
                ApplicationExtension ae;

                size_t block_size = bytes[idx++]; // always 11

                for (int i = 0; i < 8; ++i) ae.appid[i] = bytes[idx++]; // Application identifier
                for (int i = 0; i < 3; ++i) ae.authcode[i] = bytes[idx++]; // Application auth code
/*
                std::cerr << "APPLICATION EXTENSION" << std::endl;
                std::cerr << "App id: " << ae.appid << std::endl;
                std::cerr << "App auth code: " << ae.authcode << std::endl;
                std::cerr << std::endl;
*/
                while (true)
                {
                    size_t nbytes = bytes[idx++]; // size of a data sub-block
                    if (nbytes == 0) break;

                    std::vector<int8_t> data;

                    for (int i = 0; i < nbytes; ++i)
                    {
                        data.push_back(bytes[idx++]);
                    }

                    ae.data_blocks.push_back(data);
                }

                blocks.push_back(std::make_unique<ApplicationExtension>(ae));

                break;
            }
            case 0x01: // Plain text extension (ignored)
            {
                uint8_t skip = bytes[idx++]; // how many bytes to skip
                idx += skip;

                while (true)
                {
                    size_t nbytes = bytes[idx++]; // size of a data sub-block
                    if (nbytes == 0) break;
                    for (int i = 0; i < nbytes; ++i, ++idx)
                        ;
                }
/*
                std::cerr << "PLAIN TEXT EXTENSION" << std::endl;
                std::cerr << std::endl;
*/
                break;
            }
            case 0xFE: // Comment extension
            {
                CommentBlock cb;

                //std::cerr << "COMMENT EXTENSION" << std::endl;

                while (true)
                {
                    size_t nbytes = bytes[idx++]; // size of a data sub-block
                    if (nbytes == 0) break;

                    std::string comment = "";

                    for (int i = 0; i < nbytes; ++i)
                    {
                        comment += char(bytes[idx++]);
                    }

                    cb.comments.push_back(comment);

                    //std::cerr << comment << std::endl;
                }

                //std::cerr << std::endl;

                blocks.push_back(std::make_unique<CommentBlock>(cb));

                break;
            }
            default:
            {
                std::cerr << "WTF is this extension: " << HexToString(label) << std::endl;
                done = true;
                break;
            }
            }
            break;
        }
        case 0x3B: // Trailer
        {
            std::cerr << "Finished reading GIF data!" << std::endl;
            done = true;
            break;
        }
        default:
        {
            std::cerr << "WTF: " << HexToString(b) << std::endl;
            done = true;
            break;
        }
        }
    }

    std::cerr << std::endl;
    std::cerr << "LIST OF BLOCKS" << std::endl;

    for (int i = 0; i < blocks.size(); ++i)
    {
        std::cerr << block_type_str[blocks[i]->type] << std::endl;
    }

    if (SDL_Init(SDL_INIT_EVERYTHING) < 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL: %s", SDL_GetError());
        std::exit(1);
    }

    SDL_Window* window = SDL_CreateWindow("GIF Viewer",
                                          SDL_WINDOWPOS_UNDEFINED,
                                          SDL_WINDOWPOS_UNDEFINED,
                                          canvas_width,
                                          canvas_height,
                                          SDL_WINDOW_SHOWN);
    if (window == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create window: %s", SDL_GetError());
        std::exit(1);
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create renderer: %s", SDL_GetError());
        std::exit(1);
    }

    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_STREAMING, canvas_width, canvas_height);
    if (texture == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create texture: %s", SDL_GetError());
        std::exit(1);
    }

    int i = 0; // index for blocks list
    bool loop = false;
    int delay = 0; // animation rate in milliseconds
    int disposal = 2;
    bool doRender = false;
    int img_left = 0;
    int img_top = 0;
    int img_width = 0;
    int img_height = 0;
    bool transparent = false;
    uint8_t trans_idx = 0;

    Color bkgd = gct_flag ? gct[bkgd_color_idx] : Color{255, 255, 255};
    uint32_t bkgd_color = color_rgba(bkgd.r, bkgd.g, bkgd.b, 255);

    std::vector<uint32_t> pixels(canvas_width * canvas_height, bkgd_color);
    std::vector<uint32_t> prev;

    SDL_Event event;

    bool quit = false;

    while (!quit)
    {
        GIFBlock* block = blocks[i].get();
        i = (i + 1) % blocks.size(); // TODO

        if (block->type == BT_IMAGE)
        {
            Image* img = dynamic_cast<Image*>(block);

            img_left = img->left;
            img_top = img->top;
            img_width = img->width;
            img_height = img->height;

            int row1[img_height];
            int row2[img_height];

            if (img->interlace)
            {
                for (int i = 0; i < img_height; ++i)
                    row1[i] = i;
                int j = 0;
                for (int i = 0; i < img_height; i += 8, j++)  /* Interlace Pass 1 */
                    row2[i] = row1[j];
                for (int i = 4; i < img_height; i += 8, j++)  /* Interlace Pass 2 */
                    row2[i] = row1[j];
                for (int i = 2; i < img_height; i += 4, j++)  /* Interlace Pass 3 */
                    row2[i] = row1[j];
                for (int i = 1; i < img_height; i += 2, j++)  /* Interlace Pass 4 */
                    row2[i] = row1[j];
            }
            else
            {
                for (int i = 0; i < img_height; ++i)
                    row1[i] = row2[i] = i;
            }

            for (int y = 0; y < img_height; ++y)
            {
                for (int x = 0; x < img_width; ++x)
                {
                    int index = img->index[row2[y] * img_width + x];

                    if ((transparent && index != trans_idx) || ! transparent)
                    {
                        Color c = img->ct[index];
                        int offset = (img_top + y) * canvas_width + (img_left + x);
                        pixels[offset] = color_rgba(c.r, c.g, c.b, 255);
                    }
                }
            }

            doRender = true;
        }
        else if (block->type == BT_GRAPHIC_CONTROL)
        {
            GraphicsControl* gc = dynamic_cast<GraphicsControl*>(block);

            disposal = gc->disposal_method;
            transparent = gc->transparent;
            delay = gc->delay_time * 10; // TODO what to do when 0?

            if (transparent)
            {
                trans_idx = gc->color_index;
            }
        }
        else if (block->type == BT_APPLICATION_EXTENSION)
        {
            ApplicationExtension* ae = dynamic_cast<ApplicationExtension*>(block);
            // TODO
        }
        else // comment
        {
            CommentBlock* ce = dynamic_cast<CommentBlock*>(block);

            for (auto comment : ce->comments)
            {
                std::cerr << comment << std::endl;
            }

            std::cerr << std::endl;
        }

        if (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_QUIT:
            {
                quit = true;
                break;
            }
            }
        }

        if (!doRender)
        {
            continue;
        }

        SDL_UpdateTexture(texture, nullptr, &pixels[0], canvas_width * 4);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);

        switch (disposal)
        {
        case 0: // disposal method not specified
        case 1: // do not dispose of graphic
        {
            break;
        }
        case 2: // overwrite graphic with background color
        {
            for (int y = 0; y < img_height; ++y)
            {
                for (int x = 0; x < img_width; ++x)
                {
                    int offset = (img_top + y) * canvas_width + (img_left + x);
                    pixels[offset] = bkgd_color;
                }
            }
            break;
        }
        case 3: // overwrite graphic with previous graphic
        {
            pixels = prev;
            break;
        }
        }

        prev = pixels;
        doRender = false;

        SDL_Delay(delay);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}