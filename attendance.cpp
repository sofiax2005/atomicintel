#include "httplib.h"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <fstream>
#include <regex>
#include <curl/curl.h>

using json = nlohmann::json;

struct AttendanceRecord {
    int userId;
    std::string timestamp;
    double latitude, longitude;
    std::string role;
};

class AcademicCalendar {
private:
    std::vector<std::string> holidays;
    std::vector<std::string> extraClasses;
public:
    void loadFromJSON(const json& j) {
        holidays = j.value("holidays", std::vector<std::string>{});
        extraClasses = j.value("extraClasses", std::vector<std::string>{});
    }
    bool isHoliday(const std::string& date) const {
        return std::find(holidays.begin(), holidays.end(), date) != holidays.end();
    }
    bool hasExtraClass(const std::string& date) const {
        return std::find(extraClasses.begin(), extraClasses.end(), date) != extraClasses.end();
    }
    AcademicCalendar& operator+(const std::string& extraClassDate) {
        extraClasses.push_back(extraClassDate);
        return *this;
    }
};

class User {
protected:
    int userId;
    std::string name, role;
public:
    User(int id, const std::string& n, const std::string& r) : userId(id), name(n), role(r) {}
    virtual ~User() = default;
    virtual void markAttendance(double lat, double lon, const AcademicCalendar& cal, const std::string& date) = 0;
    virtual json generateReport() const = 0;
    static bool isValidUserId(const std::string& id) {
        std::regex pattern("(STU|TCH)[0-9]+");
        return std::regex_match(id, pattern);
    }
};

class Student : public User {
public:
    Student(int id, const std::string& n) : User(id, n, "Student") {}
    void markAttendance(double lat, double lon, const AcademicCalendar& cal, const std::string& date) override {
        if (cal.isHoliday(date) && !cal.hasExtraClass(date)) {
            throw std::invalid_argument("Cannot mark on holiday without extra class (Student)");
        }
        if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
            throw std::invalid_argument("Invalid geolocation (VPN suspected)");
        }
    }
    json generateReport() const override {
        return {{"id", userId}, {"name", name}, {"role", role}, {"type", "Student Report"}};
    }
};

class Teacher : public User {
public:
    Teacher(int id, const std::string& n) : User(id, n, "Teacher") {}
    void markAttendance(double lat, double lon, const AcademicCalendar& cal, const std::string& date) override {
        if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
            throw std::invalid_argument("Invalid geolocation (VPN suspected)");
        }
    }
    json generateReport() const override {
        return {{"id", userId}, {"name", name}, {"role", role}, {"type", "Teacher Report"}};
    }
};

template <typename T>
class AttendanceQueue {
private:
    std::queue<T> records;
    std::mutex mtx;
public:
    void enqueue(const T& record) {
        std::lock_guard<std::mutex> lock(mtx);
        records.push(record);
    }
    std::vector<T> flush() {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<T> flushed;
        while (!records.empty()) {
            flushed.push_back(records.front());
            records.pop();
        }
        return flushed;
    }
};

std::string getCurrentDate() {
    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local = *std::localtime(&time);
    std::ostringstream oss;
    oss << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

int main() {
    httplib::Server svr;
    sqlite3* db;
    if (sqlite3_open("attendance.db", &db) != SQLITE_OK) {
        std::cerr << "Failed to open DB\n";
        return 1;
    }
    const char* createTable = "CREATE TABLE IF NOT EXISTS attendance (userId INT, timestamp TEXT, lat REAL, lon REAL, role TEXT);";
    sqlite3_exec(db, createTable, nullptr, nullptr, nullptr);

    AcademicCalendar cal;
    try {
        std::ifstream f("calendar.json");
        json j;
        f >> j;
        cal.loadFromJSON(j);
    } catch (...) {
        std::cerr << "Failed to load calendar\n";
    }

    AttendanceQueue<AttendanceRecord> queue;

    svr.Get("/calendar", [&](const httplib::Request& req, httplib::Response& res) {
        json j = {{"holidays", cal.holidays}, {"extraClasses", cal.extraClasses}};
        res.set_content(j.dump(), "application/json");
    });

    svr.Post("/mark-attendance", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            json data = json::parse(req.body);
            int userId = data["userId"];
            std::string role = data["role"];
            double lat = data["lat"];
            double lon = data["lon"];
            std::string date = getCurrentDate();

            User* user;
            if (role == "Student") {
                user = new Student(userId, "Name"); // In production, load from DB
            } else if (role == "Teacher") {
                user = new Teacher(userId, "Name");
            } else {
                throw std::invalid_argument("Invalid role");
            }

            user->markAttendance(lat, lon, cal, date.substr(0, 10));
            std::string sql = "INSERT INTO attendance (userId, timestamp, lat, lon, role) VALUES (" + std::to_string(userId) + ", '" + date + "', " + std::to_string(lat) + ", " + std::to_string(lon) + ", '" + role + "');";
            sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);

            json response = {{"status", "success"}, {"report", user->generateReport()}};
            res.set_content(response.dump(), "application/json");
            delete user;
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    svr.Get("/reports", [&](const httplib::Request& req, httplib::Response& res) {
        json reports = json::array();
        const char* sql = "SELECT * FROM attendance;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                json record = {
                    {"userId", sqlite3_column_int(stmt, 0)},
                    {"timestamp", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))},
                    {"lat", sqlite3_column_double(stmt, 2)},
                    {"lon", sqlite3_column_double(stmt, 3)},
                    {"role", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4))}
                };
                reports.push_back(record);
            }
        }
        sqlite3_finalize(stmt);
        res.set_content(reports.dump(), "application/json");
    });

    svr.Post("/sync", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            json data = json::parse(req.body);
            for (const auto& item : data) {
                int userId = item["userId"];
                std::string timestamp = item["timestamp"];
                double lat = item["lat"];
                double lon = item["lon"];
                std::string role = item["role"];
                std::string sql = "INSERT INTO attendance (userId, timestamp, lat, lon, role) VALUES (" + std::to_string(userId) + ", '" + timestamp + "', " + std::to_string(lat) + ", " + std::to_string(lon) + ", '" + role + "');";
                sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
            }
            res.set_content(json({{"status", "synced"}}).dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    std::cout << "Backend running on port 8080\n";
    svr.listen("0.0.0.0", 8080);

    sqlite3_close(db);
    return 0;
}