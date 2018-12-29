/*
 * Copyright 2009 - 2010 Kevin Ackley (kackley@gwi.net)
 *
 * Permission is hereby granted, free of charge, to any person or organization
 * obtaining a copy of the software and accompanying documentation covered by
 * this license (the "Software") to use, reproduce, display, distribute,
 * execute, and transmit the Software, and to prepare derivative works of the
 * Software, and to permit third-parties to whom the Software is furnished to
 * do so, all subject to the following:
 *
 * The copyright notices in the Software and this entire statement, including
 * the above license grant, this restriction and the following disclaimer,
 * must be included in all copies of the Software, in whole or in part, and
 * all derivative works of the Software, unless such copies or derivative
 * works are solely in the form of machine-executable object code generated by
 * a source language processor.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 * FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "CheckedFile.h"
#include "E57FormatImpl.h"
#include "E57Version.h"
#include "E57XmlParser.h"
#include "ImageFileImpl.h"

namespace e57 {

#ifdef E57_DEBUG
   void E57FileHeader::dump(int indent, std::ostream& os) const
   {
      os << space(indent) << "fileSignature:      "
         << fileSignature[0] << fileSignature[1] << fileSignature[2] << fileSignature[3]
         << fileSignature[4] << fileSignature[5] << fileSignature[6] << fileSignature[7] << std::endl;
      os << space(indent) << "majorVersion:       " << majorVersion << std::endl;
      os << space(indent) << "minorVersion:       " << minorVersion << std::endl;
      os << space(indent) << "filePhysicalLength: " << filePhysicalLength << std::endl;
      os << space(indent) << "xmlPhysicalOffset:  " << xmlPhysicalOffset << std::endl;
      os << space(indent) << "xmlLogicalLength:   " << xmlLogicalLength << std::endl;
      os << space(indent) << "pageSize:           " << pageSize << std::endl;
   }
#endif

   ImageFileImpl::ImageFileImpl( ReadChecksumPolicy policy )
      : isWriter_( false ),
        writerCount_(0),
        readerCount_(0),
        checksumPolicy( std::max( 0, std::min( policy, 100 ) ) ),
        file_(nullptr),
        xmlLogicalOffset_( 0 ),
        xmlLogicalLength_( 0 ),
        unusedLogicalStart_( 0 )
   {
      /// First phase of construction, can't do much until have the ImageFile object.
      /// See ImageFileImpl::construct2() for second phase.
   }

   void ImageFileImpl::construct2(const ustring& fileName, const ustring& mode)
   {
      /// Second phase of construction, now we have a well-formed ImageFile object.

#ifdef E57_MAX_VERBOSE
      cout << "ImageFileImpl() called, fileName=" << fileName << " mode=" << mode << endl;
#endif
      unusedLogicalStart_ = sizeof(E57FileHeader);
      fileName_ = fileName;

      /// Get shared_ptr to this object
      std::shared_ptr<ImageFileImpl> imf = shared_from_this();

      //??? allow "rw" or "a"?
      if (mode == "w")
         isWriter_ = true;
      else if (mode == "r")
         isWriter_ = false;
      else
         throw E57_EXCEPTION2(E57_ERROR_BAD_API_ARGUMENT, "mode=" + ustring(mode));

      /// If mode is read, do it
      file_ = nullptr;
      if (!isWriter_) {
         try { //??? should one try block cover whole function?
            /// Open file for reading.
            file_ = new CheckedFile( fileName_, CheckedFile::ReadOnly, checksumPolicy );

            std::shared_ptr<StructureNodeImpl> root(new StructureNodeImpl(imf));
            root_ = root;
            root_->setAttachedRecursive();

            E57FileHeader header;
            readFileHeader(file_, header);

            ///!!! stash major,minor numbers for API?
            xmlLogicalOffset_ = file_->physicalToLogical(header.xmlPhysicalOffset);
            xmlLogicalLength_ = header.xmlLogicalLength;
         } catch (...) {
            /// Remember to close file if got any exception
            if (file_ != nullptr) {
               delete file_;
               file_ = nullptr;
            }
            throw;  // rethrow
         }

         try {
            /// Create parser state, attach its event handers to the SAX2 reader
            E57XmlParser parser(imf);

            parser.init();

            /// Create input source (XML section of E57 file turned into a stream).
            E57XmlFileInputSource xmlSection(file_, xmlLogicalOffset_, xmlLogicalLength_);

            unusedLogicalStart_ = sizeof(E57FileHeader);

            /// Do the parse, building up the node tree
            parser.parse( xmlSection );
         } catch (...) {
            if (file_ != nullptr) {
               delete file_;
               file_ = nullptr;
            }
            throw;  // rethrow
         }
      } else { /// open for writing (start empty)
         try {
            /// Open file for writing, truncate if already exists.
            file_ = new CheckedFile( fileName_, CheckedFile::WriteCreate, checksumPolicy );

            std::shared_ptr<StructureNodeImpl> root(new StructureNodeImpl(imf));
            root_ = root;
            root_->setAttachedRecursive();

            unusedLogicalStart_ = sizeof(E57FileHeader);
            xmlLogicalOffset_ = 0;
            xmlLogicalLength_ = 0;
         } catch (...) {
            /// Remember to close file if got any exception
            if (file_ != nullptr) {
               delete file_;
               file_ = nullptr;
            }
            throw;  // rethrow
         }
      }
   }

   void ImageFileImpl::readFileHeader(CheckedFile* file, E57FileHeader& header)
   {
#ifdef E57_DEBUG
      /// Double check that compiler thinks sizeof header is what it is supposed to be
      if (sizeof(E57FileHeader) != 48)
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL, "headerSize=" + toString(sizeof(E57FileHeader)));
#endif

      /// Fetch the file header
      file->read(reinterpret_cast<char*>(&header), sizeof(header));
#ifdef E57_MAX_VERBOSE
      header.dump(); //???
#endif

      /// Check signature
      if (strncmp(header.fileSignature, "ASTM-E57", 8) != 0)
         throw E57_EXCEPTION2(E57_ERROR_BAD_FILE_SIGNATURE, "fileName="+file->fileName());

      /// Check file version compatibility
      if (header.majorVersion > E57_FORMAT_MAJOR) {
         throw E57_EXCEPTION2(E57_ERROR_UNKNOWN_FILE_VERSION,
                              "fileName=" + file->fileName()
                              + " header.majorVersion=" + toString(header.majorVersion)
                              + " header.minorVersion=" + toString(header.minorVersion));
      }

      /// If is a prototype version (majorVersion==0), then minorVersion has to match too.
      /// In production versions (majorVersion==E57_FORMAT_MAJOR), should be able to handle any minor version.
      if (header.majorVersion == E57_FORMAT_MAJOR &&
          header.minorVersion > E57_FORMAT_MINOR) {
         throw E57_EXCEPTION2(E57_ERROR_UNKNOWN_FILE_VERSION,
                              "fileName=" + file->fileName()
                              + " header.majorVersion=" + toString(header.majorVersion)
                              + " header.minorVersion=" + toString(header.minorVersion));
      }

      /// Check if file length matches actual physical length
      if (header.filePhysicalLength != file->length(CheckedFile::Physical)) {
         throw E57_EXCEPTION2(E57_ERROR_BAD_FILE_LENGTH,
                              "fileName=" + file->fileName()
                              + " header.filePhysicalLength=" + toString(header.filePhysicalLength)
                              + " file->length=" + toString(file->length(CheckedFile::Physical)));
      }

      /// Check that page size is correct constant
      if (header.majorVersion != 0 &&
          header.pageSize != CheckedFile::physicalPageSize)
         throw E57_EXCEPTION2(E57_ERROR_BAD_FILE_LENGTH, "fileName=" + file->fileName());
   }

   void ImageFileImpl::incrWriterCount()
   {
      writerCount_++;
   }

   void ImageFileImpl::decrWriterCount()
   {
      writerCount_--;
#ifdef E57_MAX_DEBUG
      if (writerCount_ < 0) {
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL,
                              "fileName=" + fileName_
                              + " writerCount=" + toString(writerCount_)
                              + " readerCount=" + toString(readerCount_));
      }
#endif
   }

   void ImageFileImpl::incrReaderCount()
   {
      readerCount_++;
   }

   void ImageFileImpl::decrReaderCount()
   {
      readerCount_--;
#ifdef E57_MAX_DEBUG
      if (readerCount_ < 0) {
         throw E57_EXCEPTION2(E57_ERROR_INTERNAL,
                              "fileName=" + fileName_
                              + " writerCount=" + toString(writerCount_)
                              + " readerCount=" + toString(readerCount_));
      }
#endif
   }

   std::shared_ptr<StructureNodeImpl> ImageFileImpl::root()
   {
      checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__);
      return(root_);
   }

   void ImageFileImpl::close()
   {
      //??? check if already closed
      //??? flush, close

      /// If file already closed, have nothing to do
      if (file_ == nullptr)
         return;

      if (isWriter_) {
         /// Go to end of file, note physical position
         xmlLogicalOffset_ = unusedLogicalStart_;
         file_->seek(xmlLogicalOffset_, CheckedFile::Logical);
         uint64_t xmlPhysicalOffset = file_->position(CheckedFile::Physical);
         *file_ << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
#ifdef E57_OXYGEN_SUPPORT //???
         //???        *file_ << "<?oxygen RNGSchema=\"file:/C:/kevin/astm/DataFormat/xif/las_v0_05.rnc\" type=\"compact\"?>\n";
#endif

         //??? need to add name space attributes to e57Root
         root_->writeXml(shared_from_this(), *file_, 0, "e57Root");

         /// Pad XML section so length is multiple of 4
         while ((file_->position(CheckedFile::Logical) - xmlLogicalOffset_) % 4 != 0)
            *file_ << " ";

         /// Note logical length
         xmlLogicalLength_ = file_->position(CheckedFile::Logical) - xmlLogicalOffset_;

         /// Init header contents
         E57FileHeader header;

         memcpy(&header.fileSignature, "ASTM-E57", 8);

         header.majorVersion       = E57_FORMAT_MAJOR;
         header.minorVersion       = E57_FORMAT_MINOR;
         header.filePhysicalLength = file_->length(CheckedFile::Physical);
         header.xmlPhysicalOffset  = xmlPhysicalOffset;
         header.xmlLogicalLength   = xmlLogicalLength_;
         header.pageSize           = CheckedFile::physicalPageSize;
#ifdef E57_MAX_VERBOSE
         header.dump(); //???
#endif

         /// Write header at beginning of file
         file_->seek(0);
         file_->write(reinterpret_cast<char*>(&header), sizeof(header));

         file_->close();
      }

      delete file_;
      file_ = nullptr;
   }

   void ImageFileImpl::cancel()
   {
      /// If file already closed, have nothing to do
      if (file_ == nullptr)
         return;

      /// Close the file and ulink (delete) it.
      /// It is legal to cancel a read file, but file isn't deleted.
      if (isWriter_)
         file_->unlink();
      else
         file_->close();

      delete file_;
      file_ = nullptr;
   }

   bool ImageFileImpl::isOpen() const
   {
      return (file_ != nullptr);
   }

   bool ImageFileImpl::isWriter() const
   {
      return isWriter_;
   }

   int ImageFileImpl::writerCount() const
   {
      return writerCount_;
   }

   int ImageFileImpl::readerCount() const
   {
      return readerCount_;
   }

   ImageFileImpl::~ImageFileImpl()
   {
      /// Try to cancel if not already closed, but don't allow any exceptions to propogate to caller (because in dtor).
      /// If writing, this will unlink the file, so make sure call ImageFileImpl::close explicitly before dtor runs.
      try {
         cancel();
      } catch (...) {};

      /// Just in case cancel failed without freeing file_, do free here.
      if (file_ != nullptr) {
         delete file_;
         file_ = nullptr;
      }
   }

   uint64_t ImageFileImpl::allocateSpace(uint64_t byteCount, bool doExtendNow)
   {
      uint64_t oldLogicalStart = unusedLogicalStart_;

      /// Reserve space at end of file
      unusedLogicalStart_ += byteCount;

      /// If caller won't write to file immediately, it should request that the file be extended with zeros here
      if (doExtendNow)
         file_->extend(unusedLogicalStart_);

      return(oldLogicalStart);
   }

   CheckedFile* ImageFileImpl::file() const
   {
      return file_;
   }

   ustring ImageFileImpl::fileName() const
   {
      // don't checkImageFileOpen, since need to get fileName to report not open
      return fileName_;
   }

   void ImageFileImpl::extensionsAdd(const ustring& prefix, const ustring& uri)
   {
      checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__);
      //??? check if prefix characters ok, check if uri has a double quote char (others?)

      /// Check to make sure that neither prefix or uri is already defined.
      ustring dummy;
      if (extensionsLookupPrefix(prefix, dummy))
         throw E57_EXCEPTION2(E57_ERROR_DUPLICATE_NAMESPACE_PREFIX, "prefix=" + prefix + " uri=" + uri);
      if (extensionsLookupUri(uri, dummy))
         throw E57_EXCEPTION2(E57_ERROR_DUPLICATE_NAMESPACE_URI, "prefix=" + prefix + " uri=" + uri);;

      /// Append at end of list
      nameSpaces_.emplace_back(prefix, uri);
   }

   bool ImageFileImpl::extensionsLookupPrefix(const ustring& prefix, ustring& uri) const
   {
      checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__);

      /// Linear search for matching prefix
      std::vector<NameSpace>::const_iterator it;
      for (it = nameSpaces_.begin(); it < nameSpaces_.end(); ++it) {
         if (it->prefix == prefix) {
            uri = it->uri;
            return(true);
         }
      }
      return(false);
   }

   bool ImageFileImpl::extensionsLookupUri(const ustring& uri, ustring& prefix) const
   {
      checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__);

      /// Linear search for matching URI
      std::vector<NameSpace>::const_iterator it;
      for (it = nameSpaces_.begin(); it < nameSpaces_.end(); ++it) {
         if (it->uri == uri) {
            prefix = it->prefix;
            return(true);
         }
      }
      return(false);
   }

   size_t ImageFileImpl::extensionsCount() const
   {
      checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__);
      return(nameSpaces_.size());
   }

   ustring ImageFileImpl::extensionsPrefix(const size_t index) const
   {
      checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__);
      return(nameSpaces_[index].prefix);  //??? throw e57 exception here if out of bounds?
   }

   ustring ImageFileImpl::extensionsUri(const size_t index) const
   {
      checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__);
      return(nameSpaces_[index].uri);  //??? throw e57 exception here if out of bounds?
   }

   bool ImageFileImpl::isElementNameExtended(const ustring& elementName)
   {
      /// don't checkImageFileOpen

      /// Make sure doesn't have any "/" in it
      size_t found = elementName.find_first_of('/');
      if (found != std::string::npos)
         return(false);

      ustring prefix, localPart;
      try {
         /// Throws if elementName bad
         elementNameParse(elementName, prefix, localPart);
      } catch(E57Exception& /*ex*/) {
         return(false);
      }

      /// If get here, the name was good, so test if found a prefix part
      return(prefix.length() > 0);
   }

   bool ImageFileImpl::isElementNameLegal(const ustring& elementName, bool allowNumber)
   {
#ifdef E57_MAX_VERBOSE
      //cout << "isElementNameLegal elementName=""" << elementName << """" << endl;
#endif
      try {
         checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__);

         /// Throws if elementName bad
         checkElementNameLegal(elementName, allowNumber);
      } catch(E57Exception& /*ex*/) {
         return(false);
      }

      /// If get here, the name was good
      return(true);
   }

   bool ImageFileImpl::isPathNameLegal(const ustring& pathName)
   {
#ifdef E57_MAX_VERBOSE
      //cout << "isPathNameLegal elementName=""" << pathName << """" << endl;
#endif
      try {
         checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__);

         /// Throws if pathName bad
         pathNameCheckWellFormed(pathName);
      } catch(E57Exception& /*ex*/) {
         return(false);
      }

      /// If get here, the name was good
      return(true);
   }

   void ImageFileImpl::checkElementNameLegal(const ustring& elementName, bool allowNumber)
   {
      /// no checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__)

      ustring prefix, localPart;

      /// Throws if bad elementName
      elementNameParse(elementName, prefix, localPart, allowNumber);

      /// If has prefix, it must be registered
      ustring uri;
      if (prefix.length() > 0 && !extensionsLookupPrefix(prefix, uri))
         throw E57_EXCEPTION2(E57_ERROR_BAD_PATH_NAME, "elementName=" + elementName + " prefix=" + prefix);
   }

   void ImageFileImpl::elementNameParse(const ustring& elementName, ustring& prefix, ustring& localPart, bool allowNumber)
   {
      /// no checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__)

      //??? check if elementName is good UTF-8?

      size_t len = elementName.length();

      /// Empty name is bad
      if (len == 0)
         throw E57_EXCEPTION2(E57_ERROR_BAD_PATH_NAME, "elementName=" + elementName);
      unsigned char c = elementName[0];

      /// If allowing numeric element name, check if first char is digit
      if (allowNumber && '0'<=c && c<='9') {
         /// All remaining characters must be digits
         for (size_t i = 1; i < len; i++) {
            c = elementName[i];
            if (!('0'<=c && c<='9'))
               throw E57_EXCEPTION2(E57_ERROR_BAD_PATH_NAME, "elementName=" + elementName);
         }
         return;
      }

      /// If first char is ASCII (< 128), check for legality
      /// Don't test any part of a multi-byte code point sequence (c >= 128).
      /// Don't allow ':' as first char.
      if (c<128 && !(('a'<=c && c<='z') || ('A'<=c && c<='Z') || c=='_'))
         throw E57_EXCEPTION2(E57_ERROR_BAD_PATH_NAME, "elementName=" + elementName);

      /// If each following char is ASCII (<128), check for legality
      /// Don't test any part of a multi-byte code point sequence (c >= 128).
      for (size_t i = 1; i < len; i++) {
         c = elementName[i];
         if (c<128 && !(('a'<=c && c<='z') || ('A'<=c && c<='Z') || c=='_' || c==':' || ('0'<=c && c<='9') || c=='-' || c=='.'))
            throw E57_EXCEPTION2(E57_ERROR_BAD_PATH_NAME, "elementName=" + elementName);
      }

      /// Check if has at least one colon, try to split it into prefix & localPart
      size_t found = elementName.find_first_of(':');
      if (found != std::string::npos) {
         /// Check doesn't have two colons
         if (elementName.find_first_of(':', found+1) != std::string::npos)
            throw E57_EXCEPTION2(E57_ERROR_BAD_PATH_NAME, "elementName=" + elementName);

         /// Split element name at the colon
         /// ??? split before check first/subsequent char legal?
         prefix    = elementName.substr(0, found);
         localPart = elementName.substr(found+1);

         if (prefix.length() == 0 || localPart.length() == 0) {
            throw E57_EXCEPTION2(E57_ERROR_BAD_PATH_NAME,
                                 "elementName=" + elementName +
                                 " prefix=" + prefix +
                                 " localPart=" + localPart);
         }
      } else {
         prefix = "";
         localPart = elementName;
      }
   }

   void ImageFileImpl::pathNameCheckWellFormed(const ustring& pathName)
   {
      /// no checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__)

      /// Just call pathNameParse() which throws if not well formed
      bool isRelative;
      std::vector<ustring> fields;
      pathNameParse(pathName, isRelative, fields);
   }

   void ImageFileImpl::pathNameParse(const ustring& pathName, bool& isRelative, std::vector<ustring>& fields)
   {
#ifdef E57_MAX_VERBOSE
      cout << "pathNameParse pathname=""" << pathName << """" << endl;
#endif
      /// no checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__)

      /// Clear previous contents of fields vector
      fields.clear();

      size_t start = 0;

      /// Check if absolute path
      if (pathName[start] == '/')
      {
         isRelative = false;
         start = 1;
      }
      else
      {
         isRelative = true;
      }

      /// Save strings in between each forward slash '/'
      /// Don't ignore whitespace
      while (start < pathName.size()) {
         size_t slash = pathName.find_first_of('/', start);

         /// Get element name from in between '/', check valid
         ustring elementName = pathName.substr(start, slash-start);
         if (!isElementNameLegal(elementName))
         {
            throw E57_EXCEPTION2(E57_ERROR_BAD_PATH_NAME, "pathName=" + pathName + " elementName=" + elementName);
         }

         /// Add to list
         fields.push_back(elementName);

         if (slash == std::string::npos)
            break;

         /// Handle case when pathname ends in /, e.g. "/foo/", add empty field at end of list
         if (slash == pathName.size()-1)
         {
            fields.emplace_back("");
            break;
         }

         /// Skip over the slash and keep going
         start = slash + 1;
      }

      /// Empty relative path is not allowed
      if (isRelative && fields.empty())
         throw E57_EXCEPTION2(E57_ERROR_BAD_PATH_NAME, "pathName=" + pathName);

#ifdef E57_MAX_VERBOSE
      cout << "pathNameParse returning: isRelative=" << isRelative << " fields.size()=" << fields.size() << " fields=";
      for (int i = 0; i < fields.size(); i++)
         cout << fields[i] << ",";
      cout << endl;
#endif
   }

   ustring ImageFileImpl::pathNameUnparse( bool isRelative, const std::vector<ustring> &fields )
   {
      ustring path;

      if ( !isRelative )
      {
         path.push_back( '/' );
      }

      for ( unsigned i = 0; i < fields.size(); ++i )
      {
         path.append( fields.at( i ) );

         if ( i < fields.size() - 1 )
         {
            path.push_back( '/' );
         }
      }

      return path;
   }

   void ImageFileImpl::checkImageFileOpen(const char* srcFileName, int srcLineNumber, const char* srcFunctionName) const
   {
      if (!isOpen()) {
         throw E57Exception(E57_ERROR_IMAGEFILE_NOT_OPEN,
                            "fileName=" + fileName(),
                            srcFileName,
                            srcLineNumber,
                            srcFunctionName);
      }
   }

   void ImageFileImpl::dump(int indent, std::ostream& os) const
   {
      /// no checkImageFileOpen(__FILE__, __LINE__, __FUNCTION__)
      os << space(indent) << "fileName:    " << fileName_ << std::endl;
      os << space(indent) << "writerCount: " << writerCount_ << std::endl;
      os << space(indent) << "readerCount: " << readerCount_ << std::endl;
      os << space(indent) << "isWriter:    " << isWriter_ << std::endl;
      for (size_t i=0; i < extensionsCount(); i++)
         os << space(indent) << "nameSpace[" << i << "]: prefix=" << extensionsPrefix(i) << " uri=" << extensionsUri(i) << std::endl;
      os << space(indent) << "root:      " << std::endl;
      root_->dump(indent+2, os);
   }

   unsigned ImageFileImpl::bitsNeeded(int64_t minimum, int64_t maximum)
   {
      /// Relatively quick way to compute ceil(log2(maximum - minimum + 1)));
      /// Uses only integer operations and is machine independent (no assembly code).
      /// Find the bit position of the first 1 (from left) in the binary form of stateCountMinus1.
      ///??? move to E57Utility?

      uint64_t stateCountMinus1 = maximum - minimum;
      unsigned log2 = 0;
      if (stateCountMinus1 & 0xFFFFFFFF00000000LL) {
         stateCountMinus1 >>= 32;
         log2 += 32;
      }
      if (stateCountMinus1 & 0xFFFF0000LL) {
         stateCountMinus1 >>= 16;
         log2 += 16;
      }
      if (stateCountMinus1 & 0xFF00LL) {
         stateCountMinus1 >>= 8;
         log2 += 8;
      }
      if (stateCountMinus1 & 0xF0LL) {
         stateCountMinus1 >>= 4;
         log2 += 4;
      }
      if (stateCountMinus1 & 0xCLL) {
         stateCountMinus1 >>= 2;
         log2 += 2;
      }
      if (stateCountMinus1 & 0x2LL) {
         stateCountMinus1 >>= 1;
         log2 += 1;
      }
      if (stateCountMinus1 & 1LL)
         log2++;
      return(log2);
   }

}
