from flask import Flask, render_template, jsonify
import boto3
from boto3.dynamodb.conditions import Key
import time

app = Flask(__name__)

dynamodb = boto3.resource('dynamodb', region_name='eu-central-1')
table = dynamodb.Table('IoTData2')

def fetch_data():
    try:
        response = table.scan()
        data = response.get('Items', [])
        return sorted(data, key=lambda x: x.get('timestamp', 0), reverse=True)
    except Exception as e:
        print(f"Error fetching data: {e}")
        return []

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/data')
def data():
    records = fetch_data()
    return jsonify(records)

@app.route('/test_dynamodb')
def test_dynamodb():
    try:
        data = fetch_data()
        return jsonify(data)
    except Exception as e:
        return jsonify({"error": str(e)})

if __name__ == "__main__":
    app.run(debug=True)