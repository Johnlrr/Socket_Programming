#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <chrono>
#include <map>
#include <thread>
#include <algorithm>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mutex>
#include <signal.h>

#pragma comment(lib, "Ws2_32.lib")

#define PORT 8080
#define BUFFER_SIZE 1024
#define INPUT_FILE "input.txt"
#define DOWNLOADED_FILE_LIST "downloaded_files.txt"

using namespace std;

struct FileInfo {
    string name;
    uint32_t size;
};

map<string, int> priorities = { {"CRITICAL", 10}, {"HIGH", 4}, {"NORMAL", 1} };
mutex downloadQueueMutex;
vector<pair<string, string>> downloadQueue;
set<string> downloadedFiles;
map<string, uint32_t> fileSizes;
map<string, uint32_t> bytesReceived;
map<string, uint32_t> lastPercentage;

vector<pair<string, string>> readFileList(const string& filename, const set<string>& downloadedFiles) {
    vector<pair<string, string>> fileList;
    ifstream file(filename);
    string line;

    while (getline(file, line)) {
        if (line.empty()) {
            continue;
        }

        size_t pos = line.find(" ");
        if (pos != string::npos) {
            string name = line.substr(0, pos);
            string priority = line.substr(pos + 1);

            if (downloadedFiles.find(name) == downloadedFiles.end()) {
                fileList.push_back({ name, priority });
            }
        }
    }

    return fileList;
}

set<string> readDownloadedFiles() {
    set<string> downloadedFiles;
    ifstream file(DOWNLOADED_FILE_LIST);

    if (!file.is_open()) {
        return downloadedFiles;
    }
    string line;

    while (getline(file, line)) {
        if (!line.empty()) {
            downloadedFiles.insert(line);
            cout << "Read downloaded file: " << line << endl;
        }
    }

    return downloadedFiles;
}

void saveDownloadedFile(const string& fileName) {
    ofstream file(DOWNLOADED_FILE_LIST, ios::app);
    if (file.is_open()) {
        file << fileName << endl;
        file.close();
    }
    else {
        cerr << "Unable to open file: " << DOWNLOADED_FILE_LIST << endl;
    }
}

static set<string> completedFiles;

void downloadFileChunk(SOCKET sock, const string& fileName, int chunksToReceive) {
    while (bytesReceived[fileName] < fileSizes[fileName]) {
        char databuffer[BUFFER_SIZE];
        int bytesRead = recv(sock, databuffer, min(BUFFER_SIZE, fileSizes[fileName] - bytesReceived[fileName]), 0);
        if (bytesRead <= 0) {
            cerr << "Error receiving file data for " << fileName << endl;
            return;
        }

        ofstream outFile("output/" + fileName, ios::binary | ios::app);
        if (!outFile) {
            cerr << "Error opening output file for " << fileName << endl;
            return;
        }
        outFile.write(databuffer, bytesRead);
        outFile.close();

        bytesReceived[fileName] += bytesRead;

        uint32_t percentage = min(100, (bytesReceived[fileName] * 100) / fileSizes[fileName]);
        if (percentage != lastPercentage[fileName]) {
            cout << "Downloading " << fileName << "...." << percentage << "% complete" << endl;
            lastPercentage[fileName] = percentage;
        }
    }

    if (bytesReceived[fileName] == fileSizes[fileName]) {
        lock_guard<mutex> lock(downloadQueueMutex);
        if (completedFiles.find(fileName) == completedFiles.end()) {
            cout << "Completed downloading file: " << fileName << endl;
            saveDownloadedFile(fileName);
            downloadedFiles.insert(fileName);
            completedFiles.insert(fileName);

            downloadQueue.erase(remove_if(downloadQueue.begin(), downloadQueue.end(),
                [&fileName](const auto& entry) { return entry.first == fileName; }),
                downloadQueue.end());
        }
    }
}

void downloadFiles(SOCKET sock) {
    while (true) {
        vector<pair<string, string>> filesToDownload;

        {
            lock_guard<mutex> lock(downloadQueueMutex);
            filesToDownload = downloadQueue;
        }

        if (filesToDownload.empty()) {
            this_thread::sleep_for(chrono::seconds(2));
            continue;
        }

        uint32_t numFiles = filesToDownload.size();
        numFiles = htonl(numFiles);
        int sendResult = send(sock, (char*)&numFiles, sizeof(numFiles), 0);
        if (sendResult == SOCKET_ERROR) {
            cerr << "Error sending number of files\n";
            return;
        }

        // Send file name and priority with a delimiter
        for (const auto& file : filesToDownload) {
            string dataToSend = file.first + "|" + file.second;
            send(sock, dataToSend.c_str(), dataToSend.size(), 0);
        }

        for (const auto& file : filesToDownload) {
            if (fileSizes.find(file.first) == fileSizes.end()) {
                uint32_t fileSize;
                int receiveResult = recv(sock, (char*)&fileSize, sizeof(fileSize), 0);
                if (receiveResult <= 0) {
                    cerr << "Error receiving size of " << file.first << endl;
                    break;
                }
                fileSize = ntohl(fileSize);
                cout << "Receive " << file.first << " with size of " << fileSize << endl;
                fileSizes[file.first] = fileSize;
                bytesReceived[file.first] = 0;
                lastPercentage[file.first] = 0;
            }
        }

        // Concurrently download the files
        vector<thread> downloadThreads;
        for (const auto& file : filesToDownload) {
            downloadThreads.emplace_back([&, file]() {
                downloadFileChunk(sock, file.first, priorities[file.second]);
                });
        }

        for (auto& thread : downloadThreads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }
}

void scanInputFile(SOCKET sock) {
    while (true) {
        vector<pair<string, string>> filesToDownload = readFileList(INPUT_FILE, downloadedFiles);

        for (const auto& file : filesToDownload) {
            lock_guard<mutex> lock(downloadQueueMutex);
            if (completedFiles.find(file.first) == completedFiles.end() &&
                find_if(downloadQueue.begin(), downloadQueue.end(), [&file](const auto& entry) {
                    return entry.first == file.first;
                    }) == downloadQueue.end()) {
                downloadQueue.push_back(file);
                cout << "Added to download queue: " << file.first << endl;
            }
        }

        this_thread::sleep_for(chrono::seconds(2));
    }
}
void signal_callback_handler(int signum) {
    cout << "Exit..." << endl;
    exit(signum);
}
int main() {
    signal(SIGINT, signal_callback_handler);
    WSADATA wsaData;
    int result;

    result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "WSAStartup failed: " << result << endl;
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        cerr << "Socket creation error" << endl;
        WSACleanup();
        return -1;
    }

    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        cerr << "Invalid address/ Address not supported" << endl;
        return -1;
    }

    if (connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        cerr << "Connection Failed" << endl;
        return -1;
    }

    string clientName;
    cout << "Enter your name: ";
    getline(cin, clientName);

    send(sock, clientName.c_str(), clientName.size(), 0);

    char buffer[BUFFER_SIZE] = { 0 };
    int valread = recv(sock, buffer, BUFFER_SIZE, 0);
    if (valread > 0) {
        cout << "Available files:\n" << string(buffer, valread) << endl;
    }

    downloadedFiles = readDownloadedFiles();

    thread inputScanner(scanInputFile, sock);
    inputScanner.detach();

    downloadFiles(sock);

    closesocket(sock);
    WSACleanup();
    return 0;
}
