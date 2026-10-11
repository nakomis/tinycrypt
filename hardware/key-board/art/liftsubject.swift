// Subject mask via Vision's foreground instance mask (the "lift subject" in Photos).
// usage: swift liftsubject.swift <in.jpg> <out-mask.png>
import AppKit
import CoreImage
import Vision

let args = CommandLine.arguments
let input = URL(fileURLWithPath: args[1]), output = URL(fileURLWithPath: args[2])
let handler = VNImageRequestHandler(url: input)
let request = VNGenerateForegroundInstanceMaskRequest()
try handler.perform([request])
guard let result = request.results?.first else { fatalError("no subject found") }
let mask = try result.generateScaledMaskForImage(forInstances: result.allInstances, from: handler)
let ci = CIImage(cvPixelBuffer: mask)
let ctx = CIContext()
guard let cg = ctx.createCGImage(ci, from: ci.extent) else { fatalError("no image") }
let rep = NSBitmapImageRep(cgImage: cg)
try rep.representation(using: .png, properties: [:])!.write(to: output)
print("mask \(cg.width)x\(cg.height), instances \(result.allInstances.count)")
