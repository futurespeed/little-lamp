import gzip

fileName = "index.html"
f = open(fileName, "rb")
value = f.read()
f.close()

gz_file = gzip.GzipFile(filename=fileName, mode="wb",
    compresslevel=9, fileobj=open("index.html.gzip","wb"))
gz_file.write(value)
gz_file.close()