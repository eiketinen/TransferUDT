#pragma once
#include <string>
#include <fstream>
#include <iostream>

class DatabaseConfig
{
public:
    static std::string getDefaultDbPath()
    {
        // First check if there's a config file
        std::ifstream configFile("config.ini");
        if (configFile.is_open())
        {
            std::string line;
            while (std::getline(configFile, line))
            {
                if (line.find("DB_PATH=") == 0)
                {
                    return line.substr(8); // Return path after "DB_PATH="
                }
            }
            configFile.close();
        }

        // Return default path if no config file or no DB_PATH defined
#ifdef _WIN32
        return "C:\\ServerUDT\\server.db";
#else
        return "/var/lib/serverudt/server.db";
#endif
    }
};