/*
 * impose.cpp
 * Copyright (C) 2019 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "global.h"

using namespace pdf;

class Image {
    charbuff buf;
    int64_t width, height;
    PdfReference ref;
    Image( const Image & ) ;
    Image & operator=( const Image & ) ;
    bool is_valid;

    public:
        Image(const PdfReference &reference, const PdfObject *o) : buf(), width(0), height(0), ref(reference) {
            const PdfObjectStream *stream = o->GetStream();
            try {
                buf = stream->GetCopySafe();
                is_valid = true;
            } catch(...) {
                buf = charbuff();
                is_valid = false;
            }
            const PdfDictionary &dict = o->GetDictionary();
            if (dict.HasKey("Width") && dict.GetKey("Width")->IsNumber()) width = dict.GetKey("Width")->GetNumber();
            if (dict.HasKey("Height") && dict.GetKey("Height")->IsNumber()) height = dict.GetKey("Height")->GetNumber();
        }
        Image(Image &&other) noexcept :
            buf(std::move(other.buf)), width(other.width), height(other.height), ref(other.ref) {
            other.buf = charbuff(); is_valid = other.is_valid;
        }
        Image& operator=(Image &&other) noexcept {
            buf = std::move(other.buf); other.buf = charbuff(); ref = other.ref;
            width = other.width; height = other.height; is_valid = other.is_valid;
            return *this;
        }
        bool operator==(const Image &other) const noexcept {
            return other.width == width && is_valid && other.is_valid && other.height == height && other.buf == buf;
        }
        std::size_t hash() const noexcept { return buf.size(); }
        const PdfReference& reference() const noexcept { return ref; }
};

struct ImageHasher {
    std::size_t operator()(const Image& k) const { return k.hash(); }
};

typedef std::unordered_map<Image, std::vector<PdfReference>, ImageHasher> image_reference_map;


static PyObject*
dedup_images(PDFDoc *self, PyObject *args) {
    unsigned long count = 0;
    PdfIndirectObjectList &objects = self->doc->GetObjects();
    image_reference_map image_map;

    for (auto &k : objects) {
        if (!k->IsDictionary()) continue;
        const PdfDictionary &dict = k->GetDictionary();
        if (dictionary_has_key_name(dict, PdfName::KeyType, "XObject") && dictionary_has_key_name(dict, PdfName::KeySubtype, "Image")) {
            Image img(object_as_reference(k), k);
            auto it = image_map.find(img);
            if (it == image_map.end()) {
                std::vector<PdfReference> vals;
                image_map.insert(std::make_pair(std::move(img), std::move(vals)));
            } else (*it).second.push_back(img.reference());
        }
    }
    std::unordered_map<PdfReference, PdfReference, PdfReferenceHasher> ref_map;
    for (auto &x : image_map) {
        if (x.second.size() > 0) {
            const PdfReference &canonical_ref = x.first.reference();
            for (auto &ref : x.second) {
                if (ref != canonical_ref) {
                    ref_map[ref] = x.first.reference();
                    objects.RemoveObject(ref).reset();
                    count++;
                }
            }
        }
    }

    if (count > 0) {
        for (auto &k : objects) {
            if (!k->IsDictionary()) continue;
            PdfDictionary &dict = k->GetDictionary();
            if (dict.HasKey("Resources") && dict.GetKey("Resources")->IsDictionary()) {
                PdfDictionary &resources = dict.GetKey("Resources")->GetDictionary();
                if (!resources.HasKey("XObject") || !resources.GetKey("XObject")->IsDictionary()) continue;
                const PdfDictionary &xobject = resources.GetKey("XObject")->GetDictionary();
                PdfDictionary new_xobject = PdfDictionary(xobject);
                bool changed = false;
                for (const auto &x : xobject) {
                    if (x.second.IsReference()) {
                        try {
                            const PdfReference &r = ref_map.at(object_as_reference(x.second));
                            new_xobject.AddKey(x.first, r);
                            changed = true;
                        } catch (const std::out_of_range &err) { (void)err; continue; }
                    }
                }
                if (changed) resources.AddKey("XObject", new_xobject);
            }
        }
    }

    return Py_BuildValue("k", count);

}

PYWRAP(dedup_images)
