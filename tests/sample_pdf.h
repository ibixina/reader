#pragma once
// Minimal one-page PDF for tests: title + one text line with a citation.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

inline void writeSamplePdf(const std::string& path) {
    std::string txt = "BT /F1 18 Tf 72 720 Td (Neural Posterior Estimation) Tj ET";
    std::string content =
        "BT /F1 12 Tf 72 690 Td 15 TL (3.2 Posterior Estimation We use a recurrent "
        "history encoder for amortization [12].) Tj ET";
    std::vector<std::string> objs = {
        "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj",
        "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj",
        "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> >> endobj",
        "",
        "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj",
    };
    std::string stream = txt + "\n" + content;
    objs[3] = "4 0 obj << /Length " + std::to_string(stream.size()) + " >> stream\n" + stream +
              "\nendstream endobj";
    std::string out = "%PDF-1.4\n";
    std::vector<std::size_t> offs;
    for (auto& o : objs) {
        offs.push_back(out.size());
        out += o + "\n";
    }
    std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    char buf[32];
    for (auto o : offs) {
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", o);
        out += buf;
    }
    out += "trailer << /Size " + std::to_string(objs.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF";
    std::ofstream f(path, std::ios::binary);
    f << out;
}
