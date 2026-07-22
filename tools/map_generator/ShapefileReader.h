#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>

struct Point {
    double x = 0.0, y = 0.0;
};

struct PolygonRing {
    std::vector<Point> points;
    bool isHole = false;
};

struct ShapeObject {
    std::vector<PolygonRing> rings;
    int shapeType = 0;
};

struct FieldDesc {
    std::string name;
    char type = 'C';
    int length = 0;
    int decimal = 0;
};

class ShapefileReader {
public:
    ShapefileReader();
    ~ShapefileReader();

    bool open(const std::string& shpPath, const std::string& dbfPath);

    int getRecordCount() const { return m_recordCount; }

    const ShapeObject& getShape(int index) const { return m_shapes[index]; }

    std::string getStringField(int record, int field) const;
    int getFieldIndex(const std::string& name) const;
    int getFieldCount() const { return m_fields.size(); }
    const FieldDesc& getFieldDesc(int i) const { return m_fields[i]; }

private:
    bool readShp(const std::string& path);
    bool readDbf(const std::string& path);

    static uint32_t readBE32(const uint8_t* p);
    static int32_t readLE32(const uint8_t* p);
    static double readLEDouble(const uint8_t* p);

    std::vector<ShapeObject> m_shapes;
    std::vector<FieldDesc> m_fields;

    std::vector<uint8_t> m_dbfData;
    int m_dbfHeaderSize = 0;
    int m_dbfRecordSize = 0;
    int m_recordCount = 0;
};
