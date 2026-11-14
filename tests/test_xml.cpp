// The XML parser (core/xml.h) and gzip (importers/gzip.h): elements, attributes, text, entities, CDATA,
// comments, processing instructions, DOCTYPE, errors; ElementTree-style paths; a big document's speed;
// gzip round trips (stored, fixed and dynamic blocks; several members; damage found).
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "core/xml.h"
#include "gfx/image.h"
#include "importers/gzip.h"

static void testBasics() {
    const char* text =
        "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE set [ <!ENTITY x \"y\"> ]>\n"
        "<!-- a comment -->\n"
        "<Ableton MajorVersion=\"5\" Creator='Live 12'>\n"
        "  <LiveSet>\n"
        "    <Name Value=\"A &amp; B &lt;1&gt; &quot;q&quot; &apos;s&apos; &#65;&#x42;&#x20AC;\" />\n"
        "    <Tracks>\n"
        "      <AudioTrack Id=\"1\"><Name Value=\"one\"/></AudioTrack>\n"
        "      <MidiTrack Id=\"2\"><Name Value=\"two\"/><!-- c --><?pi stuff?></MidiTrack>\n"
        "      <AudioTrack Id=\"3\"><Name Value=\"three\"/></AudioTrack>\n"
        "    </Tracks>\n"
        "    <Text>hello <![CDATA[<raw> & stuff]]> world &amp; more</Text>\n"
        "    <Attr Value=\"a\tb\nc\r\nd\"/>\n"
        "  </LiveSet>\n"
        "</Ableton>\n";
    xml::Document doc;
    std::string error;
    CHECK(doc.parse(text, error));
    CHECK(error.empty());
    const xml::Element* root = doc.root();
    CHECK(root && root->tag == "Ableton");
    CHECK(root->get("MajorVersion") == "5");
    CHECK(root->get("Creator") == "Live 12");
    CHECK(root->attr("Missing") == nullptr);
    CHECK(root->get("Missing", "def") == "def");
    const xml::Element* live = root->find("LiveSet");
    CHECK(live && live->children.size() == 4);
    CHECK(live->text.empty());  // (indentation left out)
    CHECK(live->find("Name")->get("Value") == "A & B <1> \"q\" 's' AB\xE2\x82\xAC");
    const xml::Element* tracks = live->find("Tracks");
    CHECK(tracks->children.size() == 3);
    CHECK(tracks->children[1]->tag == "MidiTrack");
    CHECK(tracks->children[1]->parent == tracks);
    CHECK(tracks->children[1]->children.size() == 1);  // (the comment and the PI are not elements)
    CHECK(live->find("Text")->text == "hello <raw> & stuff world & more");
    CHECK(live->find("Attr")->get("Value") == "a b c d");
    // paths
    CHECK(root->find("LiveSet/Tracks/AudioTrack/Name")->get("Value") == "one");
    CHECK(root->find("LiveSet/Tracks/MidiTrack/Name")->get("Value") == "two");
    CHECK(root->find("./LiveSet/./Tracks") == tracks);
    CHECK(root->find(".") == root);
    CHECK(root->find("LiveSet/Nothing") == nullptr);
    auto audio = root->findAll("LiveSet/Tracks/AudioTrack");
    CHECK(audio.size() == 2 && audio[1]->get("Id") == "3");
    auto any = tracks->findAll("*");
    CHECK(any.size() == 3);
    auto names = root->findAll(".//Name");
    CHECK(names.size() == 4);
    CHECK(names.size() == 4 && names[3]->get("Value") == "three");
    auto inTracks = root->findAll("LiveSet//Name");
    CHECK(inTracks.size() == 4);
    auto deep = root->findAll(".//Tracks/*/Name");
    CHECK(deep.size() == 3);
    CHECK(root->find("LiveSet/Tracks/*/Name")->get("Value") == "one");
    // iter: itself and everything under it, in document order
    auto all = tracks->iter();
    CHECK(all.size() == 7 && all[0] == tracks && all[1]->tag == "AudioTrack" && all[2]->tag == "Name");
    CHECK(root->iter("Name").size() == 4);
    CHECK(root->iter("Ableton").size() == 1);
    // the first child whose rest of the path matches (ElementTree's find)
    xml::Document d2;
    CHECK(d2.parse("<a><b/><b><c v='2'/></b></a>", error));
    CHECK(d2.root()->find("b/c") && d2.root()->find("b/c")->get("v") == "2");
}

static void testErrors() {
    std::string error;
    const char* bad[] = {
        "",
        "just text",
        "<a>",
        "<a></b>",
        "<a b='1' b='2'/>",
        "<a b=1/>",
        "<a>&bogus;</a>",
        "<a/><b/>",
        "<a><!-- unclosed </a>",
        "<a x='<'/>",
        "</a>",
        "<a>&#xD800;</a>",
    };
    for (const char* s : bad) {
        xml::Document doc;
        error.clear();
        bool ok = doc.parse(s, error);
        CHECK(!ok);
        CHECK(!error.empty());
        CHECK(doc.root() == nullptr);
    }
    xml::Document doc;
    CHECK(!doc.parse("<a>\n<b>\n</c></a>", error));
    CHECK(error.find("line 3") == 0);
    // a moved document keeps its views
    xml::Document one;
    CHECK(one.parse("<a v='x &amp; y'><b>t</b></a>", error));
    xml::Document two = std::move(one);
    CHECK(two.root()->get("v") == "x & y");
    CHECK(two.root()->find("b")->text == "t");
    CHECK(xml::escape("a<b&\"c\">") == "a&lt;b&amp;&quot;c&quot;&gt;");
}

static void testBig() {
    // ~20 MB of .als-like XML: parsed in well under a few seconds
    std::string s = "<?xml version=\"1.0\"?>\n<Ableton><LiveSet><Tracks>\n";
    for (int t = 0; t < 200; t++) {
        s += "<MidiTrack Id=\"" + std::to_string(t) + "\"><Name><EffectiveName Value=\"T\" /></Name><Notes><KeyTracks>\n";
        for (int k = 0; k < 1000; k++)
            s += "\t\t<MidiNoteEvent Time=\"" + std::to_string(k) + "\" Duration=\"0.25\" Velocity=\"100\" "
                 "VelocityDeviation=\"0\" OffVelocity=\"64\" Probability=\"1\" IsEnabled=\"true\" NoteId=\"1\" />\n";
        s += "</KeyTracks></Notes></MidiTrack>\n";
    }
    s += "</Tracks></LiveSet></Ableton>\n";
    CHECK(s.size() > 20 * 1000 * 1000);
    auto t0 = std::chrono::steady_clock::now();
    xml::Document doc;
    std::string error;
    CHECK(doc.parse(std::move(s), error));
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("parsed %zu elements in %.2f s\n", doc.elementCount(), took);
    CHECK(took < 5.0);
    CHECK(doc.root()->iter("MidiNoteEvent").size() == 200000);
    auto tracks = doc.root()->findAll("LiveSet/Tracks/MidiTrack");
    CHECK(tracks.size() == 200);
    CHECK(tracks[199]->findAll("Notes/KeyTracks/MidiNoteEvent").size() == 1000);
    // and gzipped / ungzipped
    std::string text(6 * 1000 * 1000, ' ');
    for (size_t i = 0; i < text.size(); i++) text[i] = "<Value=\"0.25\" />\n\t"[(i * 7 + i / 13) % 18];
    auto packed = gzip::compress(text);
    t0 = std::chrono::steady_clock::now();
    std::string back;
    CHECK(gzip::decompress(packed.data(), packed.size(), back, error));
    took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("ungzipped %zu bytes in %.3f s\n", back.size(), took);
    CHECK(back == text);
}

static void testGzip() {
    std::string error, out;
    // empty, short, repetitive, random-ish (stored blocks)
    std::vector<std::string> samples = {"", "a", "hello hello hello hello hello", std::string(100000, 'x')};
    std::string noise;
    uint32_t r = 1;
    for (int i = 0; i < 200000; i++) {
        r = r * 1103515245 + 12345;
        noise += (char)(r >> 16);
    }
    samples.push_back(noise);
    std::string mixed;
    for (int i = 0; i < 3000; i++) mixed += "<KeyTrack Id=\"" + std::to_string(i % 37) + "\"/>\n";
    samples.push_back(mixed);
    for (auto& s : samples)
        for (int level : {0, 1, 6, 9}) {
            auto gz = gzip::compress(s, level);
            CHECK(gzip::isGzip(gz.data(), gz.size()));
            CHECK(gzip::decompress(gz.data(), gz.size(), out, error));
            CHECK(out == s);
        }
    // the zlib stream inside agrees with zlibInflate
    std::vector<uint8_t> raw(mixed.begin(), mixed.end());
    auto z = zlibDeflate(raw.data(), raw.size());
    std::vector<uint8_t> inflated;
    size_t used = 0;
    CHECK(gzip::inflateRaw(z.data() + 2, z.size() - 2, inflated, &used));
    CHECK(used == z.size() - 6);
    CHECK(std::string(inflated.begin(), inflated.end()) == mixed);
    // two members, joined; a header with a name and a comment
    auto a = gzip::compress(std::string("first ")), b = gzip::compress(std::string("second"));
    b[3] = 8 | 16;
    std::vector<uint8_t> named(b.begin(), b.begin() + 10);
    for (char c : std::string("name.xml")) named.push_back((uint8_t)c);
    named.push_back(0);
    for (char c : std::string("comment")) named.push_back((uint8_t)c);
    named.push_back(0);
    named.insert(named.end(), b.begin() + 10, b.end());
    a.insert(a.end(), named.begin(), named.end());
    CHECK(gzip::decompress(a.data(), a.size(), out, error));
    CHECK(out == "first second");
    // damage: not gzip, cut short, wrong checksum
    std::string plain = "<Ableton/>";
    CHECK(!gzip::decompress((const uint8_t*)plain.data(), plain.size(), out, error));
    auto gz = gzip::compress(mixed);
    CHECK(!gzip::decompress(gz.data(), gz.size() / 2, out, error));
    gz[gz.size() - 6] ^= 1;
    CHECK(!gzip::decompress(gz.data(), gz.size(), out, error));
    CHECK(!error.empty());
}

int main() {
    testBasics();
    testErrors();
    testGzip();
    testBig();
    TEST_MAIN_END();
}
