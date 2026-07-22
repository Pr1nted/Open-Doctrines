#include "ShapefileReader.h"
#include <algorithm>
#include <cstring>
#include <iostream>

ShapefileReader::ShapefileReader() = default;
ShapefileReader::~ShapefileReader() = default;

uint32_t ShapefileReader::readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}

int32_t ShapefileReader::readLE32(const uint8_t* p) {
    return (static_cast<int32_t>(p[0])) |
           (static_cast<int32_t>(p[1]) << 8) |
           (static_cast<int32_t>(p[2]) << 16) |
           (static_cast<int32_t>(p[3]) << 24);
}

double ShapefileReader::readLEDouble(const uint8_t* p) {
    uint64_t v = (static_cast<uint64_t>(p[0])) |
                 (static_cast<uint64_t>(p[1]) << 8) |
                 (static_cast<uint64_t>(p[2]) << 16) |
                 (static_cast<uint64_t>(p[3]) << 24) |
                 (static_cast<uint64_t>(p[4]) << 32) |
                 (static_cast<uint64_t>(p[5]) << 40) |
                 (static_cast<uint64_t>(p[6]) << 48) |
                 (static_cast<uint64_t>(p[7]) << 56);
    double d;
    memcpy(&d, &v, 8);
    return d;
}

bool ShapefileReader::open(const std::string& shpPath, const std::string& dbfPath) {
    if (!readShp(shpPath)) return false;
    if (!readDbf(dbfPath)) return false;
    return true;
}

bool ShapefileReader::readShp(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        std::cerr << "Cannot open " << path << std::endl;
        return false;
    }

    uint8_t header[100];
    if (fread(header, 1, 100, f) != 100) {
        std::cerr << "Failed to read SHP header" << std::endl;
        fclose(f);
        return false;
    }

    uint32_t fileCode = readBE32(header);
    if (fileCode != 9994) {
        std::cerr << "Invalid SHP file code: " << fileCode << std::endl;
        fclose(f);
        return false;
    }

    uint32_t fileLengthWords = readBE32(header + 24);
    int32_t shapeType = readLE32(header + 32);

    if (shapeType != 5 && shapeType != 3 && shapeType != 1) {
        std::cerr << "Unsupported shape type: " << shapeType << " (expected 5=polygon)" << std::endl;
        fclose(f);
        return false;
    }

    uint8_t recordHeader[8];
    while (fread(recordHeader, 1, 8, f) == 8) {
        uint32_t contentLengthWords = readBE32(recordHeader + 4);
        uint64_t contentLengthBytes = static_cast<uint64_t>(contentLengthWords) * 2;

        if (contentLengthBytes == 0) continue;

        std::vector<uint8_t> content(contentLengthBytes);
        if (fread(content.data(), 1, contentLengthBytes, f) != contentLengthBytes) {
            std::cerr << "Failed to read SHP record content" << std::endl;
            break;
        }

        int32_t recShapeType = readLE32(content.data());
        if (recShapeType != shapeType) continue;

        ShapeObject obj;
        obj.shapeType = shapeType;

        if (shapeType == 5) {
            int numParts = readLE32(content.data() + 36);
            int numPoints = readLE32(content.data() + 40);

            if (numParts <= 0 || numPoints <= 0) continue;

            std::vector<int> parts(numParts);
            for (int i = 0; i < numParts; ++i) {
                parts[i] = readLE32(content.data() + 44 + i * 4);
            }

            struct RingData {
                PolygonRing ring;
                double area;
            };
            std::vector<RingData> ringData;
            ringData.reserve(numParts);

            for (int i = 0; i < numParts; ++i) {
                int start = parts[i];
                int end = (i + 1 < numParts) ? parts[i + 1] : numPoints;
                if (start < 0 || end > numPoints || start >= end) continue;

                PolygonRing ring;
                ring.points.reserve(end - start);
                for (int j = start; j < end; ++j) {
                    int offset = 44 + numParts * 4 + j * 16;
                    ring.points.push_back({
                        readLEDouble(content.data() + offset),
                        readLEDouble(content.data() + offset + 8)
                    });
                }

                // Compute signed area (shoelace with antimeridian-safe deltas).
                double area = 0.0;
                int n = ring.points.size();
                for (int j = 0; j < n; ++j) {
                    int j2 = (j + 1) % n;
                    double lon1 = ring.points[j].x;
                    double lon2 = ring.points[j2].x;
                    double lat1 = ring.points[j].y;
                    double lat2 = ring.points[j2].y;
                    if (lon2 - lon1 > 180.0) lon2 -= 360.0;
                    else if (lon2 - lon1 < -180.0) lon2 += 360.0;
                    area += (lon1 * lat2 - lon2 * lat1);
                }
                ringData.push_back({std::move(ring), area});
            }

            // First ring is always an outer ring. Holes have the opposite sign.
            if (!ringData.empty()) {
                bool firstPositive = (ringData[0].area > 0);
                for (auto& rd : ringData) {
                    rd.ring.isHole = ((rd.area > 0) != firstPositive);
                    obj.rings.push_back(std::move(rd.ring));
                }
            }
        }

        m_shapes.push_back(obj);
    }

    fclose(f);
    return true;
}

bool ShapefileReader::readDbf(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        std::cerr << "Cannot open " << path << std::endl;
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    m_dbfData.resize(fileSize);
    if (fread(m_dbfData.data(), 1, fileSize, f) != static_cast<size_t>(fileSize)) {
        std::cerr << "Failed to read DBF file" << std::endl;
        fclose(f);
        return false;
    }
    fclose(f);

    if (fileSize < 32) return false;

    m_recordCount = readLE32(m_dbfData.data() + 4);
    m_dbfHeaderSize = readLE32(reinterpret_cast<const uint8_t*>(m_dbfData.data() + 8)) & 0xFFFF;
    m_dbfRecordSize = readLE32(reinterpret_cast<const uint8_t*>(m_dbfData.data() + 10)) & 0xFFFF;

    if (m_dbfHeaderSize < 32 || m_dbfRecordSize <= 0) return false;

    int fieldOffset = 32;
    while (fieldOffset + 32 <= m_dbfHeaderSize - 1) {
        if (m_dbfData[fieldOffset] == 0x0D) break;
        char name[12];
        memcpy(name, m_dbfData.data() + fieldOffset, 11);
        name[11] = 0;
        std::string fieldName(name);
        if (fieldName.empty()) break;

        FieldDesc fd;
        fd.name = fieldName;
        fd.type = static_cast<char>(m_dbfData[fieldOffset + 11]);
        fd.length = m_dbfData[fieldOffset + 16];
        fd.decimal = m_dbfData[fieldOffset + 17];
        m_fields.push_back(fd);

        fieldOffset += 32;
    }

    return true;
}

std::string ShapefileReader::getStringField(int record, int field) const {
    if (record < 0 || record >= m_recordCount) return "";
    if (field < 0 || field >= static_cast<int>(m_fields.size())) return "";

    int offset = m_dbfHeaderSize + record * m_dbfRecordSize;
    // skip deletion marker
    offset += 1;
    for (int i = 0; i < field; ++i) {
        offset += m_fields[i].length;
    }
    int len = m_fields[field].length;
    if (offset + len > static_cast<int>(m_dbfData.size())) return "";

    std::string val(reinterpret_cast<const char*>(m_dbfData.data() + offset), len);
    // Trim trailing spaces and null bytes
    while (!val.empty() && (val.back() == ' ' || val.back() == '\0')) val.pop_back();
    return val;
}

int ShapefileReader::getFieldIndex(const std::string& name) const {
    for (int i = 0; i < static_cast<int>(m_fields.size()); ++i) {
        if (m_fields[i].name == name) return i;
    }
    return -1;
}
