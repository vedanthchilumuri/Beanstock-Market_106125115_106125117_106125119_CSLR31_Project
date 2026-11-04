// Reset the saved health data. This tool is kept separate from the main app so
// it can safely remove the user files after a clear confirmation.

// Keep the Windows min/max macros from interfering with the standard library.
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Find the folder that contains this reset tool so it can remove the app data next to it.
std::filesystem::path executableDirectory() {
    // A wide-character (UTF-16) buffer of 32768 characters, which is the
    // longest path Windows allows. The wide "W" API is used so that folder
    // names with non-English characters still work.
    std::wstring executablePath(32768, L'\0');

    // GetModuleFileNameW(nullptr, ...) writes the full path of THIS .exe
    // (e.g. C:\Users\me\dsproject\reset_data.exe) into the buffer and returns
    // how many characters it wrote.
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(),
        static_cast<DWORD>(executablePath.size()));

    // length == 0           -> the call failed.
    // length >= buffer size -> the path was cut off (truncated).
    // In both cases we cannot trust the path, so we stop with an error.
    if (length == 0 || length >= executablePath.size()) {
        throw std::runtime_error("Could not determine the reset utility location.");
    }

    // Shrink the string down to the real path length (drop the unused '\0's).
    executablePath.resize(length);

    // parent_path() removes the file name, leaving only the folder:
    //   C:\...\dsproject\reset_data.exe  ->  C:\...\dsproject
    return std::filesystem::path(executablePath).parent_path();
}

}  // namespace

// Run the reset flow and remove the app data only after confirmation.
int main() {
    try {
        // ---- Step 1: work out which paths we are dealing with -------------
        // operator/ joins path pieces with the correct separator ('\').
        // L"..." are wide-string literals to match the wide path above.
        const std::filesystem::path appDirectory = executableDirectory();
        const std::filesystem::path usersDirectory = appDirectory / L"users";
        // health_data.txt is the OLD single-file format used by earlier
        // versions of the app (before each user got their own file).
        const std::filesystem::path legacyFile = appDirectory / L"health_data.txt";

        // ---- Step 2: COUNT (but do not delete yet) the user files --------
        // We count first so we can tell the user exactly what will happen
        // before asking for confirmation.
        std::size_t userFiles = 0;
        if (std::filesystem::exists(usersDirectory)) {
            // Safety check: if something called "users" exists but is a
            // normal file rather than a folder, refuse to continue.
            if (!std::filesystem::is_directory(usersDirectory)) {
                throw std::runtime_error(
                    "The users path exists but is not a directory.");
            }
            // directory_iterator walks through every entry in the folder
            // (not sub-folders). We count only regular files ending in .txt,
            // so any other files the user put there are left alone.
            for (const auto& entry :
                 // Handle directory iterator.
                 std::filesystem::directory_iterator(usersDirectory)) {
                if (entry.is_regular_file() && entry.path().extension() == L".txt") {
                    ++userFiles;
                }
            }
        }
        const bool hasLegacyFile = std::filesystem::exists(legacyFile);

        // ---- Step 3: warn the user and ask for confirmation ---------------
        std::cout << "This will permanently delete " << userFiles
                   << " user data file(s)";
        if (hasLegacyFile) {
            std::cout << " and health_data.txt";
        }
        std::cout << " next to this reset utility.\n\n"
                     "Close the health companion first. Type RESET to continue: ";
        // ("Close the health companion first" matters: if the app is still
        //  running it holds all users in memory and will write their files
        //  again the next time it saves, undoing the reset.)

        // getline reads the whole line the user typed. The comparison is
        // exact and case-sensitive: "reset", "RESET " or "y" all cancel.
        // Requiring a whole word (not just y/n) prevents accidental deletion.
        std::string confirmation;
        std::getline(std::cin, confirmation);
        if (confirmation != "RESET") {
            std::cout << "Cancelled. No files were deleted.\n";
            return 0;   // exit code 0 = finished normally
        }

        // ---- Step 4: actually delete ------------------------------------
        std::size_t deletedFiles = 0;
        if (std::filesystem::exists(usersDirectory)) {
            // First COLLECT the paths into a vector, THEN delete them.
            // WHY: deleting files from a folder while a directory_iterator is
            // still walking that same folder is unsafe — the C++ standard
            // leaves the result unspecified (entries may be skipped or the
            // iterator may misbehave). Two passes avoids that problem.
            // (Note: this vector shadows the size_t counter of the same name
            //  declared above; inside this block "userFiles" means the list.)
            std::vector<std::filesystem::path> userFiles;
            for (const auto& entry :
                 // Handle directory iterator.
                 std::filesystem::directory_iterator(usersDirectory)) {
                if (entry.is_regular_file() && entry.path().extension() == L".txt") {
                    userFiles.push_back(entry.path());
                }
            }
            // remove() returns false if the file did not exist; it throws a
            // filesystem_error if deletion fails for another reason
            // (e.g. file is locked). Either way we stop and report it.
            for (const auto& userFile : userFiles) {
                if (!std::filesystem::remove(userFile)) {
                    throw std::runtime_error("Could not delete a user data file.");
                }
                ++deletedFiles;
            }
            // Note: the (now empty) users folder itself is NOT deleted.
        }
        // Delete the old single-file data too, if present.
        if (std::filesystem::exists(legacyFile)) {
            if (!std::filesystem::remove(legacyFile)) {
                throw std::runtime_error("Could not delete health_data.txt.");
            }
            ++deletedFiles;
        }

        std::cout << "Reset complete. Deleted " << deletedFiles
                  << " data file(s). The next app launch starts with no saved users.\n";
    } catch (const std::exception& error) {
        // std::exception is the base class of runtime_error and
        // filesystem_error, so this one handler catches all of them.
        // Errors go to std::cerr (the error stream), not std::cout.
        std::cerr << "Reset failed: " << error.what() << '\n';
        return 1;   // non-zero exit code = the program failed
    }
    return 0;
}
