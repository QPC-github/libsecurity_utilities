/*
 * Copyright (c) 2000-2001 Apple Computer, Inc. All Rights Reserved.
 * 
 * The contents of this file constitute Original Code as defined in and are
 * subject to the Apple Public Source License Version 1.2 (the 'License').
 * You may not use this file except in compliance with the License. Please obtain
 * a copy of the License at http://www.apple.com/publicsource and read it before
 * using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS
 * OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES, INCLUDING WITHOUT
 * LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. Please see the License for the
 * specific language governing rights and limitations under the License.
 */


//
// osxcode - MacOS X's standard code objects
//
#include <security_utilities/osxcode.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <CoreFoundation/CFBundle.h>
#include <CoreFoundation/CFPriv.h>
#include <CoreFoundation/CFBundlePriv.h>

using namespace CodeSigning;


namespace Security {


//
// Enumerate a single file on disk.
//
void OSXCode::scanFile(const char *pathname, Signer::State &state)
{
	// open the file (well, try)
	int fd = open(pathname, O_RDONLY);
	if (fd < 0)
		UnixError::throwMe();
	
	// how big is it?
	struct stat st;
	if (fstat(fd, &st)) {
		close(fd);
		UnixError::throwMe();
	}
#if defined(LIMITED_SIGNING)
	if (st.st_size >= 0x4000)
		st.st_size = 0x4000;
#endif
	
	// map it
	void *p = mmap(NULL, st.st_size, PROT_READ, MAP_FILE, fd, 0);
	close(fd);	// done with this either way
	if (p == MAP_FAILED)
		UnixError::throwMe();
	
	// scan it
	secdebug("codesign", "scanning file %s (%ld bytes)", pathname, long(st.st_size));
	state.enumerateContents(p, st.st_size);
	
	// unmap it (ignore error)
	munmap(p, st.st_size);
}


//
// Use prefix encoding for externalizing OSXCode objects
//
OSXCode *OSXCode::decode(const char *extForm)
{
	if (!extForm || !extForm[0] || extForm[1] != ':')
		return NULL;
	switch (extForm[0]) {
	case 't':
		return new ExecutableTool(extForm+2);
	case 'b':
		return new GenericBundle(extForm+2);
	default:
		return NULL;
	}
}


//
// Produce an OSXCode for the currently running application.
//
// Note that we don't build the CFBundleRef here; we defer this to when we
// really need it for something more interesting than the base or executable paths.
// This is important because OSXCode::main() is called from various initialization
// scenarios (out of the securityd client layer), and CFBundle calls into some
// bizarrely high-level APIs to complete CFBundleGetMainBundle. Until that is fixed
// (if it ever is), this particular instance of laziness is mandatory.
//
RefPointer<OSXCode> OSXCode::main()
{
	Boolean isRealBundle;
	string path = cfString(_CFBundleCopyMainBundleExecutableURL(&isRealBundle), true);
	if (isRealBundle) {
		const char *cpath = path.c_str();
		if (const char *slash = strrchr(cpath, '/'))
			if (const char *contents = strstr(cpath, "/Contents/MacOS/"))
				if (contents + 15 == slash)
					return new ApplicationBundle(path.substr(0, contents-cpath).c_str());
		secdebug("bundle", "OSXCode::main(%s) not recognized as bundle (treating as tool)", cpath);
	}
	return new ExecutableTool(path.c_str());
}


//
// Produce an OSXCode for whatever is at a given path.
// This tries to guess at the type of OSXCode to be used.
// If you *know*, just create the suitable subclass directly.
//
RefPointer<OSXCode> OSXCode::at(const char *path)
{
	struct stat st;
	if (stat(path, &st))
		UnixError::throwMe();
	if ((st.st_mode & S_IFMT) == S_IFDIR) {	// directory - assume bundle
		return new GenericBundle(path);
	} else {
		// look for .../Contents/MacOS/<base>
		if (const char *slash = strrchr(path, '/'))
			if (const char *contents = strstr(path, "/Contents/MacOS/"))
				if (contents + 15 == slash)
					return new GenericBundle(string(path).substr(0, contents-path).c_str(), path);
		// assume tool (single executable)
		return new ExecutableTool(path);
	}
}


//
// Executable Tools
//
void ExecutableTool::scanContents(Signer::State &state) const
{
	scanFile(mPath.c_str(), state);
}

string ExecutableTool::encode() const
{
	return "t:" + mPath;
}

string ExecutableTool::canonicalPath() const
{
	return path();
}

string ExecutableTool::executablePath() const
{
	return path();
}


//
// Generic Bundles
//
GenericBundle::GenericBundle(const char *path, const char *execPath /* = NULL */)
	: mPath(path), mBundle(NULL)
{
	if (execPath)						// caller knows that one; set it
		mExecutablePath = execPath;
	secdebug("bundle", "%p GenericBundle from path %s(%s)", this, path, executablePath().c_str());
}

GenericBundle::GenericBundle(CFBundleRef bundle, const char *root /* = NULL */)
	: mBundle(bundle)
{
	assert(bundle);
	CFRetain(bundle);
	mPath = root ? root : cfString(CFBundleCopyBundleURL(mBundle), true);
	secdebug("bundle", "%p GenericBundle from bundle %p(%s)", this, bundle, mPath.c_str());
}


GenericBundle::~GenericBundle()
{
	if (mBundle)
		CFRelease(mBundle);
}


string GenericBundle::executablePath() const
{
	if (mExecutablePath.empty())
		return mExecutablePath = cfString(CFBundleCopyExecutableURL(cfBundle()), true);
	else
		return mExecutablePath;
}


CFBundleRef GenericBundle::cfBundle() const
{
	if (!mBundle) {
		secdebug("bundle", "instantiating CFBundle for %s", mPath.c_str());
		CFRef<CFURLRef> url = CFURLCreateFromFileSystemRepresentation(NULL,
			(const UInt8 *)mPath.c_str(), mPath.length(), true);
		if (!url || !(mBundle = CFBundleCreate(NULL, url)))
			CFError::throwMe();
	}
	return mBundle;
}


CFTypeRef GenericBundle::infoPlistItem(const char *name) const
{
	return CFBundleGetValueForInfoDictionaryKey(cfBundle(), CFTempString(name));
}


void GenericBundle::scanContents(Signer::State &state) const
{
	scanFile(executablePath().c_str(), state);
}

string GenericBundle::encode() const
{
	return "b:" + mPath;
}

void *GenericBundle::lookupSymbol(const char *name)
{
    CFRef<CFStringRef> cfName(CFStringCreateWithCString(NULL, name,
                                                        kCFStringEncodingMacRoman));
    if (!cfName)
		UnixError::throwMe(EBADEXEC);	// sort of
    void *function = CFBundleGetFunctionPointerForName(cfBundle(), cfName);
    if (function == NULL)
		UnixError::throwMe(EBADEXEC);	// sort of
    return function;
}

string GenericBundle::canonicalPath() const
{
	return path();
}


string GenericBundle::resource(const char *name, const char *type, const char *subdir)
{
	return cfString(CFBundleCopyResourceURL(cfBundle(),
		CFTempString(name), CFTempString(type), CFTempString(subdir)), true);
}

void GenericBundle::resources(vector<string> &paths, const char *type, const char *subdir)
{
	CFRef<CFArrayRef> cfList = CFBundleCopyResourceURLsOfType(cfBundle(),
		CFTempString(type), CFTempString(subdir));
	UInt32 size = CFArrayGetCount(cfList);
	paths.reserve(size);
	for (UInt32 n = 0; n < size; n++)
		paths.push_back(cfString(CFURLRef(CFArrayGetValueAtIndex(cfList, n)), false));
}


//
// Load management for a loadable bundle
//
void LoadableBundle::load()
{
	if (!CFBundleLoadExecutable(cfBundle()))
		CFError::throwMe();
    secdebug("bundle", "%p (%s) loaded", this, path().c_str());
}

void LoadableBundle::unload()
{
    secdebug("bundle", "%p (%s) unloaded", this, path().c_str());
	CFBundleUnloadExecutable(cfBundle());
}

bool LoadableBundle::isLoaded() const
{
	return CFBundleIsExecutableLoaded(cfBundle());
}


} // end namespace Security