//
// Copyright 2020 The Project Oak Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

use anyhow::{ensure, Context};
use fetch::get_token;
use log::info;
use oak_abi::label::Label;
use oak_client::{create_tls_channel, interceptors::label::LabelInterceptor};
use std::{fs, io};
use structopt::StructOpt;
use tonic::Request;
use treehouse_client::proto::{
    treehouse_client::TreehouseClient, TreehouseRequest, TreehouseResponse,
};

#[derive(StructOpt, Clone)]
#[structopt(about = "Treehouse Client")]
pub struct Opt {
    #[structopt(
        long,
        help = "Path to the PEM-encoded CA root certificate.",
        default_value = "../../../examples/certs/local/ca.pem"
    )]
    ca_cert: String,
}

async fn send_request(
    client: &hyper::client::Client<
        hyper_rustls::HttpsConnector<hyper::client::HttpConnector>,
        hyper::Body,
    >,
    uri: &str,
    label_bytes: &[u8],
    signature: String,
) {
    let request = hyper::Request::builder()
        .method(http::Method::GET)
        .uri(uri)
        .header(oak_abi::OAK_LABEL_HTTP_JSON_KEY, label_bytes)
        .header(oak_abi::OAK_SIGNED_CHALLENGE_HTTP_JSON_KEY, signature)
        .body(hyper::Body::empty())
        .unwrap();

    let resp = client
        .request(request)
        .await
        .expect("Error while awaiting response");

    assert_eq!(resp.status(), http::StatusCode::OK);

    log::info!("response: {:?}", resp);
    log::info!(
        "response body: {:?}",
        hyper::body::to_bytes(resp.into_body())
            .await
            .expect("could not read response body")
    );
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    env_logger::init();
    // Send a request, and wait for the response
    let label = oak_abi::label::Label::public_untrusted();
    let label_bytes = serde_json::to_string(&label)
        .context("Couldn't serialize public/untrusted label to JSON.")?
        .into_bytes();
    let opt = Opt::from_args();

    let key_pair = oak_sign::KeyPair::generate()?;
    let signature =
        oak_sign::SignatureBundle::create(oak_abi::OAK_CHALLENGE.as_bytes(), &key_pair)?;

    // Signed challenge
    let signature = oak_abi::proto::oak::identity::SignedChallenge {
        signed_hash: signature.signed_hash,
        public_key: key_pair.pkcs8_public_key(),
    };

    let path = &opt.ca_cert;
    let ca_file =
        std::fs::File::open(path).unwrap_or_else(|e| panic!("Couldn't open {}: {}", path, e));
    let mut ca = std::io::BufReader::new(ca_file);

    // Build an HTTP connector which supports HTTPS too.
    let mut http = hyper::client::HttpConnector::new();
    http.enforce_http(false);
    // Build a TLS client, using the custom CA store for lookups.
    let mut tls = rustls::ClientConfig::new();
    tls.root_store
        .add_pem_file(&mut ca)
        .expect("Couldn't load custom CA store");
    // Join the above part into an HTTPS connector.
    let https = hyper_rustls::HttpsConnector::from((http, tls));

    let client: hyper::client::Client<_, hyper::Body> =
        hyper::client::Client::builder().build(https);

    send_request(
        &client,
        "https://localhost:8080",
        &label_bytes,
        serde_json::to_string(&signature).unwrap(),
    )
    .await;

    Ok(())
}
